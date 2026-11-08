// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-APPUX S2: the grammar test of an app page, shared by the per-app cases (tests/app_grammar_test.cpp). The page is read the way a user reads it: the app's own view_draw is called on a canvas that
// records its rectangles and texts, a row is a name at the left and a value at the middle of the page on one line, the selected row is the one with the selection rectangle behind it.
// The keys go through the whole stack (the simulator's App, its bindings and App::feed_module_app), so what the OS withholds from the app is part of what is tested.

#include <doctest/doctest.h>

#include "need.h"

#include <o3module.h>
#include <o3os/app/buttons.h>
#include <o3os/app/module_ui.h>
#include <o3os/app/tools_list.h>
#include <o3os/core/action.h>
#include <o3os/model/project.h>
#include <o3os/sim/scenario.h>
#include <o3os/sim/simulator.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace o3os::test::grammar {

struct Text {
  int x, y;
  std::string s;
  std::uint32_t face, color;
};
struct Rect {
  int x, y, w, h;
  std::uint32_t color;
};
struct Log {
  std::vector<Text> texts;
  std::vector<Rect> rects;
};
struct RecCanvas {  // (the canvas first: the draw callbacks get its address back)
  o3m_canvas c;
  Log* log;
};

inline Log& log_of(o3m_canvas* c) { return *reinterpret_cast<RecCanvas*>(c)->log; }
inline void rec_rect(o3m_canvas* c, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) { log_of(c).rects.push_back({x, y, w, h, color}); }
inline void rec_outline(o3m_canvas*, int32_t, int32_t, int32_t, int32_t, uint32_t) {}
inline void rec_text(o3m_canvas* c, int32_t x, int32_t y, const char* s, uint32_t face, uint32_t color) { log_of(c).texts.push_back({x, y, s, face, color}); }
inline int32_t rec_width(const o3m_canvas*, const char* s, uint32_t face) { return static_cast<int32_t>(std::strlen(s)) * (face == O3M_FACE_DATA ? 10 : 8); }
inline void rec_polyline(o3m_canvas*, const int16_t*, uint32_t, uint32_t) {}
inline void rec_curve(o3m_canvas*, const o3m_curve*) {}

struct Row {
  std::string name, value;
};
struct Page {
  std::vector<Row> rows;
  int selected = -1;
  std::vector<std::string> texts;  // every text drawn, rows included (to look for what must not be there)
  bool operator==(const Page& o) const {
    if (rows.size() != o.rows.size() || selected != o.selected) {
      return false;
    }
    for (std::size_t i = 0; i < rows.size(); ++i) {
      if (rows[i].name != o.rows[i].name || rows[i].value != o.rows[i].value) {
        return false;
      }
    }
    return true;
  }
};

// The running app's page 0, drawn on an 800 x 360 body.
inline Page read_page(app::ModuleUi& ui) {
  Page page;
  const o3m_module* m = ui.app_module();
  if (m == nullptr || m->ui == nullptr || m->ui->view_draw == nullptr) {
    return page;
  }
  static const o3m_draw_api api = {O3M_DRAW_API_VERSION, sizeof(o3m_draw_api), rec_rect, rec_outline, rec_text, rec_width, rec_polyline, rec_curve};
  Log log;
  RecCanvas rc = {{sizeof(o3m_canvas), 0, 800, 360, 0, nullptr, &api}, &log};
  m->ui->view_draw(const_cast<void*>(ui.app_state()), 0, &rc.c);
  // The rows are a column: its selection bar is at (x + 4, y, width) and a value ends 4 px inside the bar's right edge less 4 (o3edit::draw_rows); with no bar (no row selected) the middle of the page is taken.
  int middle = rc.c.width / 2 - 4;
  int name_x = 8;
  for (const Rect& r : log.rects) {
    if (r.color == O3M_COLOR_SEL) {
      middle = r.x + r.w - 8;
      name_x = r.x + 4;
      break;
    }
  }
  for (const Text& t : log.texts) {
    page.texts.push_back(t.s);
    if (t.face != O3M_FACE_DATA || t.x + rec_width(nullptr, t.s.c_str(), t.face) != middle) {
      continue;  // not a value (a value ends 8 px inside the selection bar)
    }
    for (const Text& n : log.texts) {
      if (n.y == t.y && n.x == name_x && n.face == O3M_FACE_DATA && &n != &t) {
        page.rows.push_back({n.s, t.s});
        break;
      }
    }
  }
  for (std::size_t i = 0; i < page.rows.size() && page.selected < 0; ++i) {
    for (const Text& n : log.texts) {
      if (n.x == name_x && n.s == page.rows[i].name && n.color == O3M_COLOR_SEL_TEXT) {
        page.selected = static_cast<int>(i);
        break;
      }
    }
  }
  return page;
}

// The first whole number in a value ("+/- 8" is 8, "20" is 20); false when there is none ("MAJOR").
inline bool number_of(const std::string& v, long& out) {
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (std::isdigit(static_cast<unsigned char>(v[i])) != 0) {
      out = std::strtol(v.c_str() + i, nullptr, 10);
      return true;
    }
  }
  return false;
}

// What the case says about the app. `fine_only`: a bit per row that has no coarse step (a list, a switch): Shift + the encoder moves it like the encoder. `inert`: a bit per row that cannot move in this
// fixture (a list that follows an instrument the page was not given): the encoder changes nothing there. `pages` > 1: the first row's name of each page, to tell them apart.
struct Spec {
  unsigned fine_only = 0;
  unsigned inert = 0;
  unsigned persist_row = 0;  // the row the persistence check changes (a row that moves in this fixture)
  unsigned moves_others = 0;  // a bit per row whose change moves another row too (Morph's INSTRUMENT A re-lists SOURCE B): the "every other row is untouched" check leaves it out
  unsigned declared = 0;     // the rows the app declares (set_own_param): the OS lists none of them
  bool pads_act = false;               // the pads are the app's own keys (Tap Calc's taps change its BPM): the probe leaves them out
  bool open_from_modules = false;      // the app is not a tool (it is not in Pattern F7 TOOLS): it is opened from the MODULES list
  bool relaunch_from_modules = false;  // the kept row is the start of a launch from MODULES (a launch from TOOLS starts at the Pattern cursor: SMF Export's PATTERN)
  unsigned text_rows = 0;  // a bit per row whose value is not a number to compare (a note name): the coarse step must differ from the fine one, in the same direction
  std::vector<std::string> page_first_row;  // empty: one page
};

// The app page of `stem` on a simulator that has the shipped apps (sim_apps.h), launched from MODULES. The keys and the encoder are the simulator's.
struct AppRig {
  sim::Simulator sim;
  app::ModuleUi* ui = nullptr;

  // The simulator with the card and the shipped apps, on the MODULES screen.
  bool setup(const std::string& card_dir) {
    std::vector<sim::ScenarioCommand> cmds;
    std::string error;
    const std::string text = "document\nwait 20\ncard \"" + card_dir + "\"\napps on\naction nav.screen 12\nwait 50\n";
    if (!sim::parse_scenario(text, cmds, error) || !sim::run_scenario(cmds, sim, [](const std::string&, const sim::Simulator&) { return true; }, error)) {
      return false;
    }
    ui = sim.app().model().modules;
    return ui != nullptr;
  }
  bool launched(const std::string& stem) {
    sim.tap(app::Button::F1);
    sim.wait_ms(100);
    if (sim.app().state().screen != app::ScreenId::ModuleApp) {  // an app that edits the project or writes files asks first: F1 again
      sim.tap(app::Button::F1);
      sim.wait_ms(100);
    }
    return sim.app().state().screen == app::ScreenId::ModuleApp && ui->app_running() && stem == ui->app_name();
  }
  // From the MODULES list: no launch context, the app says so.
  bool open(const std::string& stem, const std::string& card_dir) {
    if (!setup(card_dir)) {
      return false;
    }
    unsigned at = ui->app_entry_count();
    for (unsigned i = 0; i < ui->app_entry_count(); ++i) {
      if (stem == ui->app_entry_at(i)->stem) {
        at = i;
      }
    }
    for (unsigned i = 0; i < at; ++i) {
      sim.tap(app::Button::Down);
    }
    return at < ui->app_entry_count() && launched(stem);
  }
  // From Pattern F7 TOOLS at the cursor (track 1, row 1; no block), as a user opens a tool: the app gets its launch context.
  bool open_tool(const std::string& stem, const std::string& card_dir) {
    return setup(card_dir) && launch_from_tools(stem);
  }
  // From the MODULES list: no launch context, the app starts at what the project kept.
  bool launch_from_modules(const std::string& stem) {
    sim.action(core::action_id("nav.screen"), static_cast<core::ActionArg>(app::ScreenId::Modules));
    sim.wait_ms(20);
    unsigned at = ui->app_entry_count();
    for (unsigned i = 0; i < ui->app_entry_count(); ++i) {
      if (stem == ui->app_entry_at(i)->stem) {
        at = i;
      }
    }
    if (at == ui->app_entry_count()) {
      return false;
    }
    for (unsigned i = 0; i < 12; ++i) {
      sim.tap(app::Button::Up);
    }
    for (unsigned i = 0; i < at; ++i) {
      sim.tap(app::Button::Down);
    }
    return launched(stem);
  }
  bool relaunch(const std::string& stem, bool from_modules) { return from_modules ? launch_from_modules(stem) : launch_from_tools(stem); }
  bool launch_from_tools(const std::string& stem) {
    sim.action(core::action_id("nav.screen"), static_cast<core::ActionArg>(app::ScreenId::Pattern));
    sim.wait_ms(20);
    sim.tap(app::Button::F7);  // the verb menu
    sim.wait_ms(20);
    sim.pad(15);  // APPS > (pad 16) opens the TOOLS list
    sim.wait_ms(20);
    unsigned at = ui->tool_count();
    for (unsigned i = 0; i < ui->tool_count(); ++i) {
      if (stem == ui->app_entry_at(ui->tool_entry(i))->stem) {
        at = i;
      }
    }
    if (at == ui->tool_count()) {
      return false;
    }
    for (unsigned i = 0; i < ui->tool_count(); ++i) {
      sim.tap(app::Button::Up);  // the list opens on the row it was left on (OS-PTOOLS2 S1): from the top
    }
    for (unsigned i = 0; i < at; ++i) {
      sim.tap(app::Button::Down);
    }
    return launched(stem);
  }
  bool on_page() const { return sim.app().state().screen == app::ScreenId::ModuleApp && ui->app_running() && !ui->app_stopped(); }
  void key(app::Button b, bool shift = false) {
    if (shift) {
      sim.press(app::Button::Shift);
      sim.step();
    }
    sim.tap(b);
    if (shift) {
      sim.release(app::Button::Shift);
      sim.step();
    }
  }
  void turn(int detents, bool shift = false) {
    if (shift) {
      sim.press(app::Button::Shift);
      sim.step();
    }
    sim.encoder(detents);
    if (shift) {
      sim.release(app::Button::Shift);
      sim.step();
    }
  }
  Page page() { return read_page(*ui); }
  // Up to the first row, then down to row `r`.
  void select(unsigned r) {
    for (unsigned i = 0; i < 12; ++i) {
      key(app::Button::Up);
    }
    for (unsigned i = 0; i < r; ++i) {
      key(app::Button::Down);
    }
  }
};

// The grammar of OS-APPUX 3.2 on the running page of `rig`: the row, the page, the value, the coarse step, the default, and nothing else changes a value.
inline void check_grammar(AppRig& rig, const Spec& spec) {
  NEED(rig.on_page());
  const Page start = rig.page();
  NEED(start.rows.size() >= 1u);
  NEED(start.selected == 0);
  // OS-PTOOLS2 S6: in the TOOLS panel the cursor stays inside the declared rows (the target rows and INSTRUMENT are not shown there)
  const bool panel = rig.sim.app().state().modules.panel;
  const unsigned shown = static_cast<unsigned>(start.rows.size());
  const unsigned rows = panel && spec.declared < shown ? spec.declared : shown;

  // no line of keys: the key row names them (a hint line would still say F8 or ENCODER)
  for (const std::string& t : start.texts) {
    CHECK_MESSAGE(t.find("F8") == std::string::npos, "hint line: " << t);
    CHECK_MESSAGE(t.find("ENCODER") == std::string::npos, "hint line: " << t);
    CHECK_MESSAGE(t.find("UP / DOWN") == std::string::npos, "hint line: " << t);
  }

  // Up / Down: the previous / next row, stopping at the ends (no wrap)
  rig.key(app::Button::Up);
  CHECK(rig.page().selected == 0);
  for (unsigned r = 1; r < rows; ++r) {
    rig.key(app::Button::Down);
    CHECK_MESSAGE(rig.page().selected == static_cast<int>(r), "Down to row " << r);
  }
  rig.key(app::Button::Down);
  CHECK_MESSAGE(rig.page().selected == static_cast<int>(rows) - 1, "Down at the last row stays");
  for (unsigned r = rows - 1; r > 0; --r) {
    rig.key(app::Button::Up);
    CHECK_MESSAGE(rig.page().selected == static_cast<int>(r) - 1, "Up to row " << r - 1);
  }
  const Page home = rig.page();
  CHECK_MESSAGE(home == start, "going down and up again changes nothing else");

  // Left / Right: the page (none with one page: nothing moves)
  if (spec.page_first_row.size() <= 1u) {
    rig.key(app::Button::Left);
    rig.key(app::Button::Right);
    CHECK_MESSAGE(rig.page() == start, "Left / Right on a one-page app do nothing");
  } else {
    const unsigned pages = static_cast<unsigned>(spec.page_first_row.size());
    rig.key(app::Button::Left);
    CHECK_MESSAGE(rig.page().rows[0].name == spec.page_first_row[0], "Left on the first page stays");
    for (unsigned p = 1; p < pages; ++p) {
      rig.key(app::Button::Right);
    }
    CHECK_MESSAGE(rig.page().rows[0].name == spec.page_first_row[pages - 1], "Right to the last page");
    rig.key(app::Button::Right);
    CHECK_MESSAGE(rig.page().rows[0].name == spec.page_first_row[pages - 1], "Right on the last page stays");
    CHECK_MESSAGE(rig.page().selected >= 0, "the cursor is on a row of the new page");
    for (unsigned p = 1; p < pages; ++p) {
      rig.key(app::Button::Left);
    }
    CHECK_MESSAGE(rig.page().rows[0].name == spec.page_first_row[0], "Left back to the first page");
    CHECK(rig.page() == start);
  }

  // the encoder, Shift + the encoder and Delete, row by row (on page 0, and on the last page of a paged app)
  const unsigned pages = spec.page_first_row.empty() ? 1u : static_cast<unsigned>(spec.page_first_row.size());
  for (unsigned pass = 0; pass < (pages > 1u ? 2u : 1u); ++pass) {
    const unsigned p = pass == 0u ? 0u : pages - 1u;
    for (unsigned i = 0; i < p; ++i) {
      rig.key(app::Button::Right);
    }
    const Page base = rig.page();
    const unsigned n = panel && spec.declared < base.rows.size() ? spec.declared : static_cast<unsigned>(base.rows.size());
    for (unsigned r = 0; r < n; ++r) {
      rig.select(r);
      const unsigned bit = (p == 0u ? 1u : 0x100u) << r;  // (the bits of a second page are the high byte)
      const std::string v0 = rig.page().rows[r].value;
      CAPTURE(base.rows[r].name);
      CAPTURE(p);
      int dir = 0;
      rig.turn(1);
      if (rig.page().rows[r].value != v0) {
        dir = 1;
      } else {
        rig.turn(-1);  // (the first turn moved nothing: the row is at its top, so the other way)
        if (rig.page().rows[r].value != v0) {
          dir = -1;
        }
      }
      NEED(rig.page().selected == static_cast<int>(r));
      if ((spec.inert & bit) != 0u) {
        CHECK_MESSAGE(dir == 0, "an inert row moved");
        continue;
      }
      NEED(dir != 0);
      const std::string plain = rig.page().rows[r].value;
      // every other row is untouched
      Page moved = rig.page();
      for (unsigned o = 0; o < n; ++o) {
        if (o != r && (spec.moves_others & bit) == 0u) {
          CHECK(moved.rows[o].value == base.rows[o].value);
        }
      }
      // Delete: the row's default (a row starts at its default)
      rig.key(app::Button::Delete);
      CHECK_MESSAGE(rig.page().rows[r].value == v0, "Delete restores " << v0 << " (was " << plain << ")");
      // Shift + the encoder in the same direction: coarse, or the same where the row has no coarse step
      rig.turn(dir, true);
      const std::string coarse = rig.page().rows[r].value;
      long a = 0, b = 0, c = 0;
      if ((spec.text_rows & bit) != 0u) {
        CHECK_MESSAGE(coarse != v0, "Shift + encoder moves the value");
        CHECK_MESSAGE(((spec.fine_only & bit) != 0u) == (coarse == plain), "coarse " << coarse << " against the fine " << plain << " from " << v0);
      } else if (number_of(v0, a) && number_of(plain, b) && number_of(coarse, c)) {
        const long fine = std::labs(b - a);
        const long big = std::labs(c - a);
        if ((spec.fine_only & bit) != 0u) {
          CHECK_MESSAGE(big == fine, "no coarse step: Shift + encoder reads as the encoder");
        } else {
          CHECK_MESSAGE(big > fine, "coarse " << coarse << " is wider than the fine " << plain << " from " << v0);
        }
      } else {
        CHECK_MESSAGE(coarse == plain, "a list moves one entry, Shift or not");
      }
      CHECK_MESSAGE(rig.page().selected == static_cast<int>(r), "Shift + encoder keeps the row");
      rig.key(app::Button::Delete);
      CHECK(rig.page().rows[r].value == v0);
    }
    for (unsigned i = 0; i < p; ++i) {
      rig.key(app::Button::Left);
    }
  }
  rig.select(0);
  CHECK_MESSAGE(rig.page() == start, "every value is back at its default");

  // nothing else changes a value: a value is moved off its default first, so that a reset would show too
  rig.select(0);
  rig.turn(1);
  rig.turn(-1);
  rig.turn(1);
  const Page moved = rig.page();
  struct Probe {
    app::Button b;
    bool shift;
  };
  const Probe probes[] = {{app::Button::Delete, true}, {app::Button::Up, true},  {app::Button::Down, true}, {app::Button::Left, true},
                          {app::Button::Right, true},  {app::Button::Copy, false}, {app::Button::Copy, true},  {app::Button::Insert, false},
                          {app::Button::Insert, true}, {app::Button::F4, false},   {app::Button::F5, false},    {app::Button::F6, false},
                          {app::Button::F7, false},    {app::Button::F4, true},    {app::Button::F5, true},     {app::Button::F6, true}};
  for (const Probe& pr : probes) {
    NEED(rig.on_page());
    if (pr.b == app::Button::F7 && !pr.shift) {
      continue;  // F7 with F8 is the chord that leaves: F7 alone is free, but the probe stays clear of the chord
    }
    rig.key(pr.b, pr.shift);
    const Page after = rig.page();
    CHECK_MESSAGE(after == moved, "key " << static_cast<int>(pr.b) << (pr.shift ? " with Shift" : "") << " changed the page");
  }
  for (unsigned pad : {0u, 7u, 47u}) {
    if (spec.pads_act) {
      break;
    }
    rig.sim.pad(pad);
    CHECK_MESSAGE(rig.page() == moved, "pad " << pad << " changed the page");
  }
  rig.key(app::Button::Delete);
  CHECK_MESSAGE(rig.page() == start, "Delete puts the first row back");
}

// A changed row is kept with the project (set_own_param, kept by the pass) and is back when the app is left and launched again; its Delete puts the default back and that is kept as well.
inline void check_persistence(AppRig& rig, const std::string& stem, const Spec& spec) {
  NEED(rig.on_page());
  const unsigned r = spec.persist_row;
  rig.select(r);
  const std::string base = rig.page().rows[r].value;
  rig.turn(2);
  const std::string changed = rig.page().rows[r].value;
  CHECK_MESSAGE(changed != base, "the row moved from " << base);
  for (int i = 0; i < 4; ++i) {
    rig.sim.step();  // (the pass that keeps the values with the project)
  }
  const model::Project* project = rig.sim.project();
  NEED(project != nullptr);
  const model::AppParams* kept = project->find_app_params(stem.c_str(), stem.size());
  NEED(kept != nullptr);
  CHECK(kept->count == spec.declared);
  // out and in again: the page opens on the kept value, with nothing reset by enter()
  rig.key(app::Button::F8);
  CHECK(!rig.ui->app_running());
  NEED(rig.relaunch(stem, spec.relaunch_from_modules));
  CHECK_MESSAGE(rig.page().rows[r].value == changed, "after a relaunch the row reads " << rig.page().rows[r].value << ", not " << changed);
  // the default is kept too
  rig.select(r);
  rig.key(app::Button::Delete);
  CHECK(rig.page().rows[r].value == base);
  for (int i = 0; i < 4; ++i) {
    rig.sim.step();
  }
  rig.key(app::Button::F8);
  NEED(rig.relaunch(stem, spec.relaunch_from_modules));
  CHECK(rig.page().rows[r].value == base);
  // and a screen button leaves the app as F8 does (the settings are the project's, nothing is lost)
  rig.select(r);
  rig.turn(1);
  const std::string again = rig.page().rows[r].value;
  for (int i = 0; i < 4; ++i) {
    rig.sim.step();
  }
  rig.key(app::Button::Song);
  CHECK(!rig.ui->app_running());
  CHECK(rig.sim.app().state().screen == app::ScreenId::Song);
  rig.sim.action(core::action_id("nav.screen"), static_cast<core::ActionArg>(app::ScreenId::Pattern));
  NEED(rig.relaunch(stem, spec.relaunch_from_modules));
  CHECK(rig.page().rows[r].value == again);
}

// ---- OS-APPUX S5: the target rows ------------------------------------------------------------------------------------------------------------------------------

inline int row_index(const Page& p, const char* name) {
  for (std::size_t i = 0; i < p.rows.size(); ++i) {
    if (p.rows[i].name == name) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

// What a tool app's target rows are (docs/specs/OS-APPUX.md 3.7): launched from Pattern F7 TOOLS at the cursor (pattern 1, track 1, row 1, no block), the app starts there; TRACK and ROW move the target and
// nothing else, Delete puts the launch value back, a pattern that does not exist reads (EMPTY), and nothing is kept: after F8 and a relaunch the rows read the cursor again. `pages_right`: the Right presses to the
// page with the rows. `writes`: the app writes notes into an empty pattern (Rhythm Gen, Chords): its APPLY lands on the moved target and nowhere else.
struct TargetSpec {
  unsigned pages_right = 0;
  bool writes = false;
};

inline void to_target_page(AppRig& rig, unsigned pages_right) {
  for (unsigned i = 0; i < 4; ++i) {
    rig.key(app::Button::Left);
  }
  for (unsigned i = 0; i < pages_right; ++i) {
    rig.key(app::Button::Right);
  }
}

inline void check_target_rows(AppRig& rig, const std::string& stem, const TargetSpec& ts) {
  NEED(rig.on_page());
  to_target_page(rig, ts.pages_right);
  Page p = rig.page();
  const int ip = row_index(p, "PATTERN"), it = row_index(p, "TRACK"), ir = row_index(p, "ROW"), in = row_index(p, "ROWS");
  NEED((ip >= 0 && it >= 0 && ir >= 0 && in >= 0));
  CHECK(p.rows[ip].value == "1");  // the Pattern cursor
  CHECK(p.rows[it].value == "1");
  CHECK(p.rows[ir].value == "1");
  const Page launch = p;
  rig.select(static_cast<unsigned>(it));
  rig.turn(2);
  rig.select(static_cast<unsigned>(ir));
  rig.turn(4);
  p = rig.page();
  CHECK(p.rows[it].value == "3");
  CHECK(p.rows[ir].value == "5");
  CHECK(p.rows[ip].value == "1");
  CHECK(p.rows[in].value == launch.rows[in].value);
  rig.select(static_cast<unsigned>(ip));
  rig.turn(198);  // a pattern the project does not have
  CHECK_MESSAGE(rig.page().rows[ip].value.find("(EMPTY)") != std::string::npos, "pattern 199: " << rig.page().rows[ip].value);
  CHECK(rig.page().rows[it].value == "3");  // (the others stay where the user put them)
  rig.key(app::Button::Delete);
  CHECK(rig.page().rows[ip].value == "1");
  // nothing is kept: out and in again from TOOLS, the rows read the cursor
  rig.key(app::Button::F8);
  CHECK(!rig.ui->app_running());
  NEED(rig.relaunch(stem, false));
  to_target_page(rig, ts.pages_right);
  const Page again = rig.page();
  NEED(again.rows.size() == launch.rows.size());
  for (std::size_t i = 0; i < again.rows.size(); ++i) {
    CHECK_MESSAGE(again.rows[i].value == launch.rows[i].value, again.rows[i].name << " reads " << again.rows[i].value << " after a relaunch, not " << launch.rows[i].value);
  }
  if (!ts.writes) {
    return;
  }
  // APPLY writes ROWS rows at track 3 from row 5, and nowhere else. Track 3 holds a note no app writes and Rhythm Gen does not take for a note (the sentinel, past O3M_NOTE_LAST): a row the APPLY wrote does not read it any more (both apps write the note of every row of
  // the target, a hit or an empty one), so the rows that changed are the rows that were written. ROWS is set to 8, which neither app starts with.
  constexpr unsigned kSentinel = 200, kTrack = 2, kFirst = 4, kRowsWanted = 8;
  const model::Pattern* shown = rig.sim.project() != nullptr ? rig.sim.project()->pattern(1) : nullptr;
  NEED(shown != nullptr);
  NEED(shown->track_length[kTrack] >= kFirst + kRowsWanted);
  model::Pattern* pat = const_cast<model::Pattern*>(shown);  // (the simulator's own document, which is not const: the test marks a track, the way a user's notes would be there)
  for (unsigned row = 0; row < 128; ++row) {
    pat->steps[kTrack][row].note = kSentinel;
  }
  const auto notes = [&rig]() {
    std::vector<unsigned> v;
    const model::Pattern* now = rig.sim.project() != nullptr ? rig.sim.project()->pattern(1) : nullptr;
    for (unsigned track = 0; now != nullptr && track < 16; ++track) {
      for (unsigned row = 0; row < 128; ++row) {
        v.push_back(now->steps[track][row].note);
      }
    }
    return v;
  };
  const std::vector<unsigned> before = notes();
  NEED(before.size() == 16u * 128u);
  rig.select(static_cast<unsigned>(it));
  rig.turn(2);
  rig.select(static_cast<unsigned>(ir));
  rig.turn(4);
  rig.select(static_cast<unsigned>(in));
  rig.turn(-1000);  // 1 ...
  rig.turn(7);      // ... and 8
  CHECK(rig.page().rows[in].value == "8");
  rig.key(app::Button::F1);
  rig.sim.step();
  const std::vector<unsigned> after = notes();
  NEED(after.size() == before.size());
  std::vector<unsigned> written;  // the rows of track 3 that no longer hold the sentinel
  unsigned elsewhere = 0;
  for (unsigned i = 0; i < after.size(); ++i) {
    if (after[i] == before[i]) {
      continue;
    }
    if (i / 128u == kTrack) {
      written.push_back(i % 128u);
    } else {
      ++elsewhere;
    }
  }
  CHECK_MESSAGE(elsewhere == 0u, "steps changed outside the target: " << elsewhere);
  CHECK_MESSAGE(written.size() == kRowsWanted, "the APPLY wrote " << written.size() << " rows of track 3, not the 8 of ROWS");
  for (unsigned k = 0; k < written.size(); ++k) {
    CHECK_MESSAGE(written[k] == kFirst + k, "the APPLY wrote row " << written[k] + 1 << " of track 3, not row " << kFirst + k + 1);
  }
}

// Mutate's and Morph's INSTRUMENT row: the instrument the Pattern shows (01 here), one step per detent, Delete the launch instrument, not kept.
inline void check_instrument_row(AppRig& rig, const std::string& stem, const char* name) {
  NEED(rig.on_page());
  Page p = rig.page();
  const int i = row_index(p, name);
  NEED(i >= 0);
  CHECK(p.rows[static_cast<std::size_t>(i)].value == "01");
  rig.select(static_cast<unsigned>(i));
  rig.turn(1);
  CHECK(rig.page().rows[static_cast<std::size_t>(i)].value == "02");
  rig.turn(100);
  CHECK(rig.page().rows[static_cast<std::size_t>(i)].value == "64");  // (stops at the last instrument)
  rig.key(app::Button::Delete);
  CHECK(rig.page().rows[static_cast<std::size_t>(i)].value == "01");
  rig.turn(3);
  rig.key(app::Button::F8);
  NEED(rig.relaunch(stem, false));
  CHECK(rig.page().rows[static_cast<std::size_t>(i)].value == "01");
}

}  // namespace o3os::test::grammar
