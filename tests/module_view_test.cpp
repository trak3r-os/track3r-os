// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-012b: the restricted draw API of the module views (module/view.h, ABI 1.1): clipping to the view rectangle, the per-view budgets (draw calls, points, text,
// display-list words, time), a misbehaving view blanked (nothing of its frame replayed) and its module quarantined, the descriptors of ABI 1.0 and 1.1 at the loader
// (a 1.0 module still loads; an app is 1.1 and has no sound entries), and the host table the audio layer lends. Host only.
#include <doctest/doctest.h>

#define NEED(cond)                                  \
  do {                                              \
    const bool need_ok_ = static_cast<bool>(cond);  \
    CHECK(need_ok_);                                \
    if (!need_ok_) {                                \
      return;                                       \
    }                                               \
  } while (0)

#include "support/module_blob.h"

#include <o3os/module/loader.h>
#include <o3os/module/view.h>
#include <o3os/platform/host/dir_storage.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace o3os::module;
using namespace o3os::module::test;
namespace fs = std::filesystem;

// ---- a recording sink ---------------------------------------------------------------------------------------------------------------
struct Event {
  enum class K { Fill, Text, Poly, Curve } kind;
  int x = 0, y = 0, w = 0, h = 0;
  unsigned color = 0;
  unsigned face = 0;
  std::string text;
  unsigned points = 0;
  CurveCall curve{};
  std::vector<std::int16_t> xy;
};

struct Sink final : ViewSink {
  std::vector<Event> ev;
  void fill(int x, int y, int w, int h, std::uint8_t color) noexcept override {
    Event e;
    e.kind = Event::K::Fill;
    e.x = x, e.y = y, e.w = w, e.h = h, e.color = color;
    ev.push_back(e);
  }
  void text(int x, int y, const char* s, std::uint8_t face, std::uint8_t color) noexcept override {
    Event e;
    e.kind = Event::K::Text;
    e.x = x, e.y = y, e.face = face, e.color = color, e.text = s;
    ev.push_back(e);
  }
  void polyline(const std::int16_t* xy, std::uint32_t points, std::uint8_t color) noexcept override {
    Event e;
    e.kind = Event::K::Poly;
    e.points = points, e.color = color;
    e.xy.assign(xy, xy + 2 * points);
    ev.push_back(e);
  }
  void curve(const CurveCall& c) noexcept override {
    Event e;
    e.kind = Event::K::Curve;
    e.curve = c;
    e.points = c.n;
    e.color = c.color;
    ev.push_back(e);
  }
};

// ---- the module under test: what its view does is a mode the test sets ------------------------------------------------------------------
std::uint32_t g_now = 0;
std::uint32_t fake_clock(void*) { return g_now; }
std::vector<std::uint16_t> g_stamps;
std::uint16_t g_stamp_now = 0x1234;
std::uint16_t stamp(std::uint16_t id) {
  g_stamps.push_back(id);
  const std::uint16_t before = g_stamp_now;
  g_stamp_now = id;
  return before;
}

std::uint32_t g_calls_wanted = 0;
std::int32_t g_w[4] = {};
enum class Mode { Shapes, Params, Calls, Points, Text, Words, SlowCalls, SlowReturn, Late, Garbage, Hang1 };
Mode g_mode = Mode::Shapes;
o3m_canvas* g_saved_canvas = nullptr;
std::uint32_t g_seen_param_count = 0;
float g_seen_params[O3M_PARAMS_MAX] = {};
std::uint32_t g_seen_view = 99;
int g_seen_w = 0, g_seen_h = 0;
int g_enters = 0, g_inputs = 0, g_exits = 0;
o3m_ui_event g_last_event{};

void view_draw(void*, std::uint32_t view, o3m_canvas* c) {
  const o3m_draw_api* api = draw_api();
  g_saved_canvas = c;
  g_seen_view = view;
  g_seen_w = c->width;
  g_seen_h = c->height;
  switch (g_mode) {
    case Mode::Shapes: {
      api->rect(c, 10, 10, 20, 20, O3M_COLOR_ACCENT);       // inside
      api->rect(c, -5, -5, 20, 20, O3M_COLOR_INSTR);        // across the corner
      api->rect(c, 500, 500, 10, 10, O3M_COLOR_FX);         // outside
      api->rect(c, 0, 0, 0, 5, O3M_COLOR_FX);               // empty
      api->rect(c, 0, 0, -3, 5, O3M_COLOR_FX);              // negative
      api->rect(c, -2000000000, 0, 2000000000, 4, O3M_COLOR_TEXT);  // ends at x 0: nothing
      api->rect(c, 5, 5, 2000000000, 2000000000, O3M_COLOR_PANE);   // far bigger than the canvas
      api->outline(c, -5, -5, 20, 20, O3M_COLOR_SEL);
      api->text(c, 3, 4, "HELLO", O3M_FACE_DATA, O3M_COLOR_TEXT);
      api->text(c, -100, 4, "GONE", O3M_FACE_DATA, O3M_COLOR_TEXT);   // wholly left of the canvas
      api->text(c, 3, 900, "GONE", O3M_FACE_DATA, O3M_COLOR_TEXT);    // wholly below it
      api->text(c, -6, 4, "EDGE", O3M_FACE_DATA, O3M_COLOR_TEXT);     // crosses the left edge: the sink's scissor cuts it
      const std::int16_t line[6] = {0, 0, 50, 50, 100, 0};
      api->polyline(c, line, 3, O3M_COLOR_VOL);
      const std::int16_t off[4] = {-50, -50, -10, -10};
      api->polyline(c, off, 2, O3M_COLOR_VOL);                         // wholly outside
      const float y[4] = {0.0f, 1.0f, 0.5f, 0.25f};
      const o3m_curve cv = {nullptr, y, 4, O3M_AXIS_LINEAR, O3M_AXIS_DB, 0.0f, 1.0f, -12.0f, 12.0f, O3M_CURVE_AXES | O3M_CURVE_FILL, O3M_COLOR_INSTR};
      api->curve(c, &cv);
      break;
    }
    case Mode::Params:
      g_seen_param_count = c->param_count;
      for (std::uint32_t i = 0; i < c->param_count; ++i) {
        g_seen_params[i] = c->params[i];
      }
      break;
    case Mode::Calls:
      for (unsigned i = 0; i < 200; ++i) {
        api->rect(c, static_cast<std::int32_t>(i % 40), 0, 1, 1, O3M_COLOR_TEXT);
      }
      break;
    case Mode::Points: {
      std::int16_t xy[200] = {};
      for (unsigned i = 0; i < 100; ++i) {
        xy[2 * i] = static_cast<std::int16_t>(i);
        xy[2 * i + 1] = static_cast<std::int16_t>(i % 7);
      }
      for (unsigned k = 0; k < 4; ++k) {
        api->polyline(c, xy, 100, O3M_COLOR_VOL);  // 400 points against 256
      }
      break;
    }
    case Mode::Text:
      for (unsigned k = 0; k < 10; ++k) {
        api->text(c, 0, static_cast<std::int32_t>(k) * 8, "0123456789012345678901234567890123456789012345678901234567890123", O3M_FACE_MICRO, O3M_COLOR_TEXT);
      }
      break;
    case Mode::Words: {
      float y[100];
      for (unsigned i = 0; i < 100; ++i) {
        y[i] = std::sin(static_cast<float>(i) * 0.1f);
      }
      const o3m_curve cv = {nullptr, y, 100, O3M_AXIS_LINEAR, O3M_AXIS_LINEAR, 0.0f, 1.0f, -1.0f, 1.0f, O3M_CURVE_AXES, O3M_COLOR_INSTR};
      for (unsigned k = 0; k < 3; ++k) {
        api->curve(c, &cv);  // each costs 2 * 100 + 16 + 200 = 416 words against 700 in all
      }
      break;
    }
    case Mode::SlowCalls:
      for (unsigned i = 0; i < 10; ++i) {
        g_now += 1500;  // 1.5 ms a call: the third one is past 4 ms
        api->rect(c, 0, 0, 4, 4, O3M_COLOR_TEXT);
      }
      break;
    case Mode::SlowReturn:
      api->rect(c, 0, 0, 4, 4, O3M_COLOR_TEXT);
      g_now += 6000;  // no call after the delay: only the return can see it
      break;
    case Mode::Late:
      api->rect(c, 0, 0, 4, 4, O3M_COLOR_TEXT);
      break;
    case Mode::Garbage:
      api->rect(nullptr, 0, 0, 4, 4, O3M_COLOR_TEXT);
      api->text(c, 0, 0, nullptr, O3M_FACE_DATA, O3M_COLOR_TEXT);                    // no string
      api->text(c, 0, 0, "x", 9, O3M_COLOR_TEXT);                                     // no such face
      api->text(c, 0, 0, "x", O3M_FACE_DATA, 99);                                     // no such colour
      api->rect(c, 0, 0, 4, 4, 16);                                                   // no such colour
      api->polyline(c, nullptr, 3, O3M_COLOR_TEXT);
      {
        const std::int16_t one[2] = {1, 1};
        api->polyline(c, one, 1, O3M_COLOR_TEXT);                                     // one point is no line
      }
      api->curve(c, nullptr);
      {
        const float y[3] = {0, 1, 2};
        const o3m_curve bad_axis = {nullptr, y, 3, 7, O3M_AXIS_LINEAR, 0.0f, 1.0f, 0.0f, 1.0f, 0, O3M_COLOR_INSTR};
        api->curve(c, &bad_axis);
        const o3m_curve bad_range = {nullptr, y, 3, O3M_AXIS_LINEAR, O3M_AXIS_LINEAR, 1.0f, 1.0f, 0.0f, 1.0f, 0, O3M_COLOR_INSTR};
        api->curve(c, &bad_range);
        const o3m_curve nan_range = {nullptr, y, 3, O3M_AXIS_LINEAR, O3M_AXIS_LINEAR, 0.0f, std::nanf(""), 0.0f, 1.0f, 0, O3M_COLOR_INSTR};
        api->curve(c, &nan_range);
        const o3m_curve bad_log = {nullptr, y, 3, O3M_AXIS_LOG_FREQ, O3M_AXIS_LINEAR, 0.0f, 100.0f, 0.0f, 1.0f, 0, O3M_COLOR_INSTR};
        api->curve(c, &bad_log);
        const o3m_curve bad_flags = {nullptr, y, 3, O3M_AXIS_LINEAR, O3M_AXIS_LINEAR, 0.0f, 1.0f, 0.0f, 1.0f, 64, O3M_COLOR_INSTR};
        api->curve(c, &bad_flags);
        const o3m_curve too_long = {nullptr, y, 129, O3M_AXIS_LINEAR, O3M_AXIS_LINEAR, 0.0f, 1.0f, 0.0f, 1.0f, 0, O3M_COLOR_INSTR};
        api->curve(c, &too_long);
      }
      break;
    case Mode::Hang1:
      break;
  }
}
void enter_fn(void*) { ++g_enters; }
void input_fn(void*, const o3m_ui_event* ev) {
  ++g_inputs;
  g_last_event = *ev;
  g_now += 5000;  // slow only when told: the test sets g_slow_input
}
void exit_fn(void*) { ++g_exits; }
int fake_init(void*, const o3m_host*) { return 0; }
void fake_shutdown(void*) {}
std::uint32_t fake_render(void*, const float*, float*, std::uint32_t) { return 0; }
void fake_set_param(void*, std::uint32_t, float) {}
void fake_note_on(void*, std::uint32_t, std::uint32_t, std::uint32_t) {}
void fake_note_off(void*, std::uint32_t) {}

const o3m_ui kUi = {sizeof(o3m_ui), enter_fn, input_fn, view_draw, exit_fn, nullptr};
const o3m_ui kUiNoOptional = {sizeof(o3m_ui), nullptr, nullptr, view_draw, nullptr, nullptr};
const o3m_param kParams[3] = {{"A", 0.0f, 1.0f, 0.5f, 0}, {"B", 0.0f, 10.0f, 5.0f, 0}, {"C", -1.0f, 1.0f, 0.0f, 0}};
const o3m_module kInstr = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_INSTRUMENT, sizeof(o3m_module), "viewer", 16, 3, kParams,
                           fake_init, fake_render, fake_set_param, fake_note_on, fake_note_off, fake_shutdown, &kUi, 2};
const o3m_module kNoViews = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_INSTRUMENT, sizeof(o3m_module), "plain", 16, 3, kParams,
                             fake_init, fake_render, fake_set_param, fake_note_on, fake_note_off, fake_shutdown, nullptr, 0};
const o3m_module kApp = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_APP, sizeof(o3m_module), "tool", 16, 3, kParams,
                         fake_init, nullptr, nullptr, nullptr, nullptr, fake_shutdown, &kUiNoOptional, 1};

struct Rig {
  ViewHost host;
  Sink sink;
  Rig() {
    g_now = 0;
    g_stamps.clear();
    g_stamp_now = 0x1234;
    ViewHost::Hooks h;
    h.now_us = fake_clock;
    h.enter = stamp;
    host.set_hooks(h);
  }
  ViewStatus draw(Mode m, int w = 100, int h = 80, const o3m_module& mod = kInstr, std::uint32_t view = 0) {
    g_mode = m;
    sink.ev.clear();
    return host.draw(mod, nullptr, 0x8042, view, w, h, nullptr, 0);
  }
};

}  // namespace

TEST_CASE("module view: clip_rect cuts to the canvas and refuses what is empty") {
  struct Case {
    int x, y, w, h;
    bool ok;
    int ex, ey, ew, eh;
  };
  const Case cases[] = {
      {10, 10, 20, 20, true, 10, 10, 20, 20},
      {-5, -5, 20, 20, true, 0, 0, 15, 15},
      {90, 70, 50, 50, true, 90, 70, 10, 10},
      {-10, 0, 10, 5, false, 0, 0, 0, 0},     // ends at 0
      {100, 0, 5, 5, false, 0, 0, 0, 0},      // starts at the edge
      {0, 0, 0, 5, false, 0, 0, 0, 0},
      {0, 0, 5, -1, false, 0, 0, 0, 0},
      {-1000000, -1000000, 3000000, 3000000, true, 0, 0, 100, 80},
      {0, 0, 100, 80, true, 0, 0, 100, 80},
  };
  for (const Case& c : cases) {
    int x = c.x, y = c.y, w = c.w, h = c.h;
    CAPTURE(c.x);
    CAPTURE(c.y);
    CHECK(clip_rect(x, y, w, h, 100, 80) == c.ok);
    if (c.ok) {
      CHECK(x == c.ex);
      CHECK(y == c.ey);
      CHECK(w == c.ew);
      CHECK(h == c.eh);
    }
  }
  int x = 0, y = 0, w = 5, h = 5;
  CHECK(!clip_rect(x, y, w, h, 0, 80));  // an empty canvas
}

TEST_CASE("module view: the shapes a view draws are clipped to its rectangle and replayed in order") {
  Rig r;
  NEED(r.draw(Mode::Shapes) == ViewStatus::Ok);
  CHECK(g_seen_view == 0u);
  CHECK(g_seen_w == 100);
  CHECK(g_saved_canvas->draw == draw_api());  // a view finds the drawing functions on its canvas
  CHECK(g_seen_h == 80);
  r.host.replay(r.sink);
  const std::vector<Event>& e = r.sink.ev;
  // rects: inside; the corner one cut to 15 x 15; the huge one cut to the canvas from (5, 5): 95 x 75; the others dropped
  size_t i = 0;
  NEED(e.size() >= 5);
  CHECK(e[i].kind == Event::K::Fill);
  CHECK((e[i].x == 10 && e[i].y == 10 && e[i].w == 20 && e[i].h == 20 && e[i].color == O3M_COLOR_ACCENT));
  ++i;
  CHECK((e[i].x == 0 && e[i].y == 0 && e[i].w == 15 && e[i].h == 15 && e[i].color == O3M_COLOR_INSTR));
  ++i;
  CHECK((e[i].x == 5 && e[i].y == 5 && e[i].w == 95 && e[i].h == 75 && e[i].color == O3M_COLOR_PANE));
  ++i;
  // the outline at (-5, -5) 20 x 20: only the bottom and the right edges are inside
  CHECK((e[i].kind == Event::K::Fill && e[i].x == 0 && e[i].y == 14 && e[i].w == 15 && e[i].h == 1 && e[i].color == O3M_COLOR_SEL));
  ++i;
  CHECK((e[i].kind == Event::K::Fill && e[i].x == 14 && e[i].y == 0 && e[i].w == 1 && e[i].h == 14 && e[i].color == O3M_COLOR_SEL));
  ++i;
  // text: HELLO whole; EDGE at x = -6 loses the E that is wholly left of the canvas and keeps DGE from x = 0; the two wholly outside are gone
  CHECK((e[i].kind == Event::K::Text && e[i].text == "HELLO" && e[i].x == 3 && e[i].y == 4 && e[i].face == O3M_FACE_DATA));
  ++i;
  CHECK((e[i].kind == Event::K::Text && e[i].text == "DGE" && e[i].x == 0 && e[i].y == 4));
  ++i;
  CHECK((e[i].kind == Event::K::Poly && e[i].points == 3 && e[i].xy[2] == 50 && e[i].color == O3M_COLOR_VOL));
  ++i;
  CHECK((e[i].kind == Event::K::Curve && e[i].points == 4 && e[i].curve.x == nullptr && e[i].curve.y[1] == 1.0f && e[i].curve.y_axis == O3M_AXIS_DB &&
         e[i].curve.flags == (O3M_CURVE_AXES | O3M_CURVE_FILL) && e[i].curve.y_lo == -12.0f && e[i].curve.x_hi == 1.0f));
  ++i;
  CHECK(i == e.size());
  CHECK(r.host.calls() == 15u);  // text_width and the dropped shapes count as calls except text_width
}

TEST_CASE("module view: the draw calls of an outline that is one row or one column, and one that is all outside") {
  struct Local {
    static void draw(void*, std::uint32_t, o3m_canvas* c) {
      const o3m_draw_api* api = draw_api();
      api->outline(c, 2, 2, 10, 1, O3M_COLOR_TEXT);   // one row
      api->outline(c, 2, 5, 1, 10, O3M_COLOR_TEXT);   // one column
      api->outline(c, 20, 20, 2, 2, O3M_COLOR_TEXT);  // 2 x 2: four pixels
      api->outline(c, 300, 300, 5, 5, O3M_COLOR_TEXT);  // outside
    }
  };
  static const o3m_ui ui = {sizeof(o3m_ui), nullptr, nullptr, Local::draw, nullptr, nullptr};
  const o3m_module mod = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_INSTRUMENT, sizeof(o3m_module), "o", 16, 0, nullptr,
                          fake_init, fake_render, fake_set_param, fake_note_on, fake_note_off, fake_shutdown, &ui, 1};
  Rig r;
  NEED(r.host.draw(mod, nullptr, 1, 0, 100, 80, nullptr, 0) == ViewStatus::Ok);
  r.host.replay(r.sink);
  // a rectangle of one or two pixels in a direction is all border: one fill each, and the one outside none
  NEED(r.sink.ev.size() == 3);
  CHECK((r.sink.ev[0].x == 2 && r.sink.ev[0].y == 2 && r.sink.ev[0].w == 10 && r.sink.ev[0].h == 1));
  CHECK((r.sink.ev[1].x == 2 && r.sink.ev[1].y == 5 && r.sink.ev[1].w == 1 && r.sink.ev[1].h == 10));
  CHECK((r.sink.ev[2].x == 20 && r.sink.ev[2].y == 20 && r.sink.ev[2].w == 2 && r.sink.ev[2].h == 2));
}

TEST_CASE("module view: the current parameter values reach the view") {
  Rig r;
  g_mode = Mode::Params;
  const float values[3] = {0.25f, 7.0f, -0.5f};
  NEED(r.host.draw(kInstr, nullptr, 1, 1, 64, 64, values, 3) == ViewStatus::Ok);
  CHECK(g_seen_view == 1u);
  CHECK(g_seen_param_count == 3u);
  CHECK(g_seen_params[0] == 0.25f);
  CHECK(g_seen_params[1] == 7.0f);
  CHECK(g_seen_params[2] == -0.5f);
}

TEST_CASE("module view: what the draw API ignores costs a call and draws nothing") {
  Rig r;
  NEED(r.draw(Mode::Garbage) == ViewStatus::Ok);
  r.host.replay(r.sink);
  CHECK(r.sink.ev.empty());
  CHECK(r.host.recorded() == 0u);
  CHECK(r.host.calls() == 13u);  // the call with no canvas is not charged to anyone
}

TEST_CASE("module view: every budget blanks the view and says which one") {
  struct Case {
    Mode mode;
    ViewStatus verdict;
  };
  const Case cases[] = {
      {Mode::Calls, ViewStatus::Calls},      {Mode::Points, ViewStatus::Points},  {Mode::Text, ViewStatus::Text},
      {Mode::Words, ViewStatus::Words},      {Mode::SlowCalls, ViewStatus::Time}, {Mode::SlowReturn, ViewStatus::Time},
  };
  for (const Case& c : cases) {
    Rig r;
    CAPTURE(static_cast<int>(c.mode));
    CHECK(r.draw(c.mode) == c.verdict);
    r.host.replay(r.sink);
    CHECK(r.sink.ev.empty());  // blanked: nothing of that frame is replayed
    CHECK(r.host.recorded() == 0u);
    CHECK(std::strlen(view_status_name(c.verdict)) > 3u);
  }
  // exactly at the limit is fine: 96 calls, then the 97th is the verdict
  {
    Rig r;
    NEED(r.draw(Mode::Shapes) == ViewStatus::Ok);
    CHECK(r.host.calls() < kViewMaxCalls);
  }
}

TEST_CASE("module view: the numbers of the budgets are the ones the SDK promises") {
  static_assert(kViewMaxCalls == 96u && kViewMaxPoints == 256u && kViewMaxText == 384u && kViewMaxUs == 4000u, "O3M_VIEW_* in o3module.h");
  CHECK(kViewMaxWords == 700u);
  CHECK(sizeof(ViewHost) < 6u * 1024u);  // the record lives in SDRAM: a few KB, never the SRAM the audio path is tight on
}

TEST_CASE("module view: a view at exactly the calls budget draws, one call more blanks it") {
  struct Local {
    static void draw(void*, std::uint32_t, o3m_canvas* c) {
      for (std::uint32_t i = 0; i < g_calls_wanted; ++i) {
        draw_api()->rect(c, 0, 0, 1, 1, O3M_COLOR_TEXT);
      }
    }
  };
  g_calls_wanted = kViewMaxCalls;
  static const o3m_ui ui = {sizeof(o3m_ui), nullptr, nullptr, Local::draw, nullptr, nullptr};
  const o3m_module mod = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_INSTRUMENT, sizeof(o3m_module), "o", 16, 0, nullptr,
                          fake_init, fake_render, fake_set_param, fake_note_on, fake_note_off, fake_shutdown, &ui, 1};
  Rig r;
  CHECK(r.host.draw(mod, nullptr, 1, 0, 100, 80, nullptr, 0) == ViewStatus::Ok);
  CHECK(r.host.calls() == kViewMaxCalls);
  g_calls_wanted = kViewMaxCalls + 1;
  CHECK(r.host.draw(mod, nullptr, 1, 0, 100, 80, nullptr, 0) == ViewStatus::Calls);
}

TEST_CASE("module view: a canvas kept after view_draw() returned draws nothing") {
  Rig r;
  NEED(r.draw(Mode::Late) == ViewStatus::Ok);
  o3m_canvas* kept = g_saved_canvas;
  NEED(kept != nullptr);
  const std::uint32_t before = r.host.recorded();
  draw_api()->rect(kept, 1, 1, 5, 5, O3M_COLOR_TEXT);
  CHECK(r.host.recorded() == before);
  r.host.replay(r.sink);
  CHECK(r.sink.ev.size() == 1u);
  draw_api()->rect(nullptr, 1, 1, 5, 5, O3M_COLOR_TEXT);  // and no canvas at all
  CHECK(draw_api()->text_width(nullptr, "abc", O3M_FACE_DATA) == 0);
}

TEST_CASE("module view: the draw table and text_width") {
  const o3m_draw_api* api = draw_api();
  CHECK(api->version == O3M_DRAW_API_VERSION);
  CHECK(api->size == sizeof(o3m_draw_api));
  Rig r;
  struct Local {
    static void draw(void*, std::uint32_t, o3m_canvas* c) {
      const o3m_draw_api* a = draw_api();
      g_w[0] = a->text_width(c, "ABCD", O3M_FACE_DISPLAY);
      g_w[1] = a->text_width(c, "ABCD", O3M_FACE_DATA);
      g_w[2] = a->text_width(c, "ABCD", 77);
      g_w[3] = a->text_width(c, nullptr, O3M_FACE_DATA);
    }
  };
  static const o3m_ui ui = {sizeof(o3m_ui), nullptr, nullptr, Local::draw, nullptr, nullptr};
  const o3m_module mod = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_INSTRUMENT, sizeof(o3m_module), "o", 16, 0, nullptr,
                          fake_init, fake_render, fake_set_param, fake_note_on, fake_note_off, fake_shutdown, &ui, 1};
  ViewHost::Metrics m;
  m.cell_w[O3M_FACE_DISPLAY] = 13;
  m.cell_w[O3M_FACE_DATA] = 7;
  r.host.set_metrics(m);
  NEED(r.host.draw(mod, nullptr, 1, 0, 100, 80, nullptr, 0) == ViewStatus::Ok);
  CHECK(g_w[0] == 52);
  CHECK(g_w[1] == 28);
  CHECK(g_w[2] == 0);
  CHECK(g_w[3] == 0);
  CHECK(r.host.calls() == 0u);  // text_width is not a draw call
}

TEST_CASE("module view: nothing is called for a view that does not exist or a canvas that is not legal") {
  Rig r;
  g_mode = Mode::Shapes;
  CHECK(r.host.draw(kNoViews, nullptr, 1, 0, 100, 80, nullptr, 0) == ViewStatus::NoView);     // no ui table
  CHECK(r.host.draw(kInstr, nullptr, 1, 2, 100, 80, nullptr, 0) == ViewStatus::NoView);       // view 2 of 2
  CHECK(r.host.draw(kInstr, nullptr, 1, 0, 15, 80, nullptr, 0) == ViewStatus::NoView);        // too narrow
  CHECK(r.host.draw(kInstr, nullptr, 1, 0, 100, 481, nullptr, 0) == ViewStatus::NoView);      // too tall
  CHECK(r.host.draw(kInstr, nullptr, 1, 0, 801, 80, nullptr, 0) == ViewStatus::NoView);
  float v[40] = {};
  CHECK(r.host.draw(kInstr, nullptr, 1, 0, 100, 80, v, 33) == ViewStatus::NoView);            // more parameters than a module has
  CHECK(r.host.draw(kInstr, nullptr, 1, 0, 100, 80, nullptr, 3) == ViewStatus::NoView);
  CHECK(r.host.draw(kInstr, nullptr, 1, 0, 100, 80, v, 3) == ViewStatus::Ok);
  r.host.replay(r.sink);
  CHECK(!r.sink.ev.empty());
  CHECK(r.host.draw(kNoViews, nullptr, 1, 0, 100, 80, nullptr, 0) == ViewStatus::NoView);
  r.sink.ev.clear();
  r.host.replay(r.sink);
  CHECK(r.sink.ev.empty());  // a refused draw leaves no old record to replay
}

TEST_CASE("module view: the crash context is stamped while view code runs and put back after") {
  Rig r;
  NEED(r.draw(Mode::Late) == ViewStatus::Ok);
  NEED(g_stamps.size() == 2u);
  CHECK(g_stamps[0] == 0x8042u);   // the module's id in
  CHECK(g_stamps[1] == 0x1234u);   // what was there before, back out
  CHECK(g_stamp_now == 0x1234u);
}

TEST_CASE("module view: an app's entry points run under the same time budget and stamp") {
  Rig r;
  const o3m_ui_event ev = {O3M_EV_BUTTON, O3M_BTN_UP, 1, O3M_MOD_SHIFT};
  g_enters = g_inputs = g_exits = 0;
  CHECK(r.host.call_enter(kInstr, nullptr, 0x8001) == ViewStatus::Ok);
  CHECK(g_enters == 1);
  CHECK(r.host.call_input(kInstr, nullptr, 0x8001, ev) == ViewStatus::Time);  // the test's input handler burns 5 ms
  CHECK(g_inputs == 1);
  CHECK((g_last_event.kind == O3M_EV_BUTTON && g_last_event.id == O3M_BTN_UP && g_last_event.value == 1 && g_last_event.mods == O3M_MOD_SHIFT));
  CHECK(r.host.call_exit(kInstr, nullptr, 0x8001) == ViewStatus::Ok);
  CHECK(g_exits == 1);
  CHECK(g_stamp_now == 0x1234u);
  // optional entries that are NULL are not called
  CHECK(r.host.call_enter(kApp, nullptr, 0x8001) == ViewStatus::NoView);
  CHECK(r.host.call_input(kApp, nullptr, 0x8001, ev) == ViewStatus::NoView);
  CHECK(r.host.call_exit(kApp, nullptr, 0x8001) == ViewStatus::NoView);
  CHECK(g_enters == 1);
  // no clock: no time budget
  ViewHost bare;
  g_inputs = 0;
  CHECK(bare.call_input(kInstr, nullptr, 0, ev) == ViewStatus::Ok);
  CHECK(g_inputs == 1);
}

TEST_CASE("module view: a module whose view overran is blocked and quarantined; its blocking goes with the set, its quarantine does not") {
  ModuleSet set;
  NEED(set.add_native("viewer", kInstr));
  NEED(set.add_native("plain", kNoViews));
  const LoadedModule* m = set.at(0);
  NEED(m != nullptr);
  CHECK(m->has_views());
  CHECK(!set.at(1)->has_views());
  CHECK(!set.views_blocked(0));
  set.block_views(0);
  CHECK(set.views_blocked(0));
  CHECK(!set.views_blocked(1));
  set.block_views(9);  // not a module of the set: ignored
  CHECK(!set.views_blocked(9));
  CHECK(set.quarantine("viewer", Status::ViewOverrun));
  CHECK(set.quarantined("viewer"));
  CHECK(std::string(status_name(Status::ViewOverrun)).find("budget") != std::string::npos);
  CHECK(!is_validation_failure(Status::ViewOverrun));  // a view fault is not a file fault
  set.clear();
  CHECK(!set.views_blocked(0));
  CHECK(set.quarantined("viewer"));
}

// ---- ABI 1.0 and 1.1 at the loader ----------------------------------------------------------------------------------------------------
namespace {

struct Card {
  fs::path root;
  Card() {
    root = fs::temp_directory_path() / ("o3m_view_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    fs::remove_all(root);
    fs::create_directories(root / "modules" / "instruments");
    fs::create_directories(root / "modules" / "apps");
  }
  ~Card() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  void write(const std::string& rel, const std::vector<std::uint8_t>& bytes) {
    std::ofstream f(root / rel, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
};

struct LoadRig {
  Card card;
  o3os::platform::host::DirStorage storage{card.root.string()};
  ModuleSet set;
  ModuleLoader loader;
  alignas(64) std::uint8_t slab[1024];
  alignas(64) std::uint8_t sdram[4096];
  LoadRig() {
    (void)storage.mount();
    set.hot().init(slab, kHotAddr, sizeof slab);
    set.cold().init(sdram, kColdAddr, sizeof sdram);
  }
  ModuleLoader::State run(const char* stem) {
    if (!loader.begin(storage, set, stem, nullptr)) {
      return ModuleLoader::State::Failed;
    }
    for (unsigned n = 0; n < 10000 && loader.state() == ModuleLoader::State::Running; ++n) {
      loader.step(4096);
    }
    return loader.state();
  }
};

// The 1.0 module: the blob of every loader test with its ABI minor put back to 0 (header and descriptor).
Blob blob_v10() {
  Blob b = good_blob();
  Desc32 d = get_desc(b);
  d.abi_minor = 0;
  put_desc(b, d);
  b.h.abi_minor = 0;
  b.seal();
  return b;
}

}  // namespace

TEST_CASE("module abi: a 1.0 and a 1.1 module still load, a 1.2 module loads and carries its version, a newer minor does not") {
  CHECK(kOsAbiMajor == 1u);
  CHECK(kOsAbiMinor == 13u);  // (OS-MODRATE S2: 1.13 the RATE bit; OS-INSFX-ABI2 A1: 1.12 published the effect surface; 1.11 the importer kind)
  {
    LoadRig r;
    r.card.write("modules/instruments/old.tmod", blob_v10().file());
    CHECK(r.run("old") == ModuleLoader::State::Done);
    NEED(r.set.count() == 1u);
    CHECK(r.set.at(0)->header.abi_minor == 0);
    CHECK(r.set.at(0)->header.module_version == 0u);  // a file below 1.2 has no version: it reads as unversioned
    CHECK(!r.set.at(0)->has_views());  // a 1.0 module's views table is validated and never called (and a host-loaded one has no native entry anyway)
  }
  {
    LoadRig r;
    r.card.write("modules/instruments/mid.tmod", blob_minor(1).file());
    CHECK(r.run("mid") == ModuleLoader::State::Done);
    NEED(r.set.count() == 1u);
    CHECK(r.set.at(0)->header.abi_minor == 1);
    CHECK(r.set.at(0)->header.module_version == 0u);
  }
  {
    LoadRig r;
    r.card.write("modules/instruments/new.tmod", blob_version(O3M_VERSION(1, 4, 2)).file());
    CHECK(r.run("new") == ModuleLoader::State::Done);
    NEED(r.set.count() == 1u);
    CHECK(r.set.at(0)->header.abi_minor == 2);
    CHECK(r.set.at(0)->header.module_version == O3M_VERSION(1, 4, 2));
  }
  {
    Blob b = blob_minor(1);  // a 1.1 file with a version in the word that was reserved: refused, and quarantined like any damaged file
    b.h.module_version = O3M_VERSION(1, 0, 0);
    b.seal();
    LoadRig r;
    r.card.write("modules/instruments/odd.tmod", b.file());
    CHECK(r.run("odd") == ModuleLoader::State::Failed);
    CHECK(r.loader.status() == Status::BadReserved);
    CHECK(r.set.quarantined("odd"));
  }
  {
    Blob b = good_blob();
    b.h.abi_minor = static_cast<std::uint8_t>(kOsAbiMinor + 1);
    b.seal();
    LoadRig r;
    r.card.write("modules/instruments/future.tmod", b.file());
    CHECK(r.run("future") == ModuleLoader::State::Failed);
    CHECK(r.loader.status() == Status::AbiMinor);
  }
}

TEST_CASE("module abi: an app is kind 4 of ABI 1.1 with no sound entries and at least one view, found in modules/apps") {
  {
    LoadRig r;
    r.card.write("modules/apps/tool.tmod", blob_app().file());
    CHECK(r.run("tool") == ModuleLoader::State::Done);
    NEED(r.set.count() == 1u);
    CHECK(r.set.at(0)->header.kind == O3M_KIND_APP);
    CHECK(r.set.at(0)->desc.render == 0u);
    CHECK(r.set.at(0)->desc.view_count == 1u);
  }
  {
    LoadRig r;  // a 1.0 file that says app: the app kind did not exist
    r.card.write("modules/apps/tool.tmod", blob_app(1, 0).file());
    CHECK(r.run("tool") == ModuleLoader::State::Failed);
    CHECK(r.loader.status() == Status::BadKind);
    CHECK(r.set.quarantined("tool"));
  }
  {
    LoadRig r;  // an app without a view is nothing
    r.card.write("modules/apps/tool.tmod", blob_app(0).file());
    CHECK(r.run("tool") == ModuleLoader::State::Failed);
    CHECK(r.loader.status() == Status::BadDescriptor);
  }
  {
    // an instrument may not leave its sound entries out
    Blob b = good_blob();
    Desc32 d = get_desc(b);
    d.render = 0;
    put_desc(b, d);
    b.relocs = {8, 12, 48, 60, 64, 72, 76, 80, 84, 96, 116};
    b.h.reloc_count = static_cast<std::uint32_t>(b.relocs.size());
    b.seal();
    LoadRig r;
    r.card.write("modules/instruments/mute.tmod", b.file());
    CHECK(r.run("mute") == ModuleLoader::State::Failed);
    CHECK(r.loader.status() == Status::BadDescriptor);
  }
  {
    LoadRig r;  // importers stay refused
    Blob b = good_blob();
    b.h.kind = O3M_KIND_IMPORTER;
    b.seal();
    r.card.write("modules/instruments/imp.tmod", b.file());
    CHECK(r.run("imp") == ModuleLoader::State::Failed);
    CHECK(r.loader.status() == Status::BadKind);
  }
}

TEST_CASE("module abi: native apps are accepted by the set under the same rules") {
  ModuleSet set;
  CHECK(set.add_native("tool", kApp));
  o3m_module no_view = kApp;
  no_view.ui = nullptr;
  no_view.view_count = 0;
  CHECK(!set.add_native("noview", no_view));
  o3m_module old_app = kApp;
  old_app.abi_minor = 0;
  CHECK(!set.add_native("oldapp", old_app));
  o3m_module no_init = kApp;
  no_init.init = nullptr;
  CHECK(!set.add_native("noinit", no_init));
  o3m_module sound_less = kInstr;
  sound_less.render = nullptr;
  CHECK(!set.add_native("mute", sound_less));
}

// OS-H1 (ADR 0014, ABI 1.3): the capability word of an app's file. The loader keeps it in the header the set holds (ModuleUi reads the capabilities from there); a file of ABI 1.2 or
// below has the word zero, whatever it is built from, and a request outside what 1.3 defines is refused and quarantined like any damaged file.
TEST_CASE("module abi: an app of ABI 1.3 carries its capabilities, every older app loads with none, and a bad request is refused") {
  {
    Blob b = blob_app(1, 3);
    b.h.reserved2[0] = O3M_CAP_MIDI_IN | O3M_CAP_EDIT_PROJECT;
    b.seal();
    LoadRig r;
    r.card.write("modules/apps/caps.tmod", b.file());
    CHECK(r.run("caps") == ModuleLoader::State::Done);
    NEED(r.set.count() == 1u);
    CHECK(r.set.at(0)->header.abi_minor == 3);
    CHECK(r.set.at(0)->header.reserved2[0] == (O3M_CAP_MIDI_IN | O3M_CAP_EDIT_PROJECT));
  }
  for (std::uint8_t minor = 1; minor <= 2; ++minor) {  // the 1.1 and 1.2 apps of the card today: the word is zero and they load as before
    LoadRig r;
    r.card.write("modules/apps/old.tmod", blob_app(1, minor).file());
    CHECK(r.run("old") == ModuleLoader::State::Done);
    NEED(r.set.count() == 1u);
    CHECK(r.set.at(0)->header.reserved2[0] == 0u);
  }
  {
    Blob b = blob_app(1, 2);  // ABI 1.2 with a capability: that word was reserved
    b.h.reserved2[0] = O3M_CAP_READ_PROJECT;
    b.seal();
    LoadRig r;
    r.card.write("modules/apps/liar.tmod", b.file());
    CHECK(r.run("liar") == ModuleLoader::State::Failed);
    CHECK(r.loader.status() == Status::BadReserved);
    CHECK(r.set.quarantined("liar"));
  }
  // C4 (ADR 0014 amendment D): a published bit this OS does not grant is refused AT LOAD with its own status, naming the capability through the header it leaves in the loader. It is a
  // clean refusal, not a bad file: nothing is quarantined, and a bit above the six published ones is still a reserved field.
  for (const std::uint32_t bit : {O3M_CAP_MIDI_OUT, O3M_CAP_AUDIO_TAP | O3M_CAP_MIDI_OUT, O3M_CAP_EDIT_PROJECT | O3M_CAP_MIDI_OUT, O3M_CAP_EDIT_PROJECT | O3M_CAP_EXPORT_FILES | O3M_CAP_MIDI_OUT}) {
    Blob b = blob_app(1, 7);  // (OS-H4: export_files is granted to a file of ABI 1.7; midi_out is the one published bit no OS grants yet)
    b.h.reserved2[0] = bit;
    b.seal();
    LoadRig r;
    r.card.write("modules/apps/later.tmod", b.file());
    CHECK(r.run("later") == ModuleLoader::State::Failed);
    CHECK(r.loader.status() == Status::CapUnsupported);
    CHECK(r.loader.module().header.reserved2[0] == bit);
    CHECK(r.set.count() == 0u);
    CHECK(!r.set.quarantined("later"));
  }
  {
    Blob b = blob_app(1, 4);  // OS-H2a: audio_tap is granted to a file of ABI 1.4 ...
    b.h.reserved2[0] = O3M_CAP_AUDIO_TAP;
    b.seal();
    LoadRig r;
    r.card.write("modules/apps/tapper.tmod", b.file());
    CHECK(r.run("tapper") == ModuleLoader::State::Done);
    NEED(r.set.count() == 1u);
    CHECK(r.set.at(0)->header.reserved2[0] == O3M_CAP_AUDIO_TAP);
  }
  {
    Blob b = blob_app(1, 7);  // OS-H4: export_files is granted to a file of ABI 1.7 ...
    b.h.reserved2[0] = O3M_CAP_READ_PROJECT | O3M_CAP_EXPORT_FILES;
    b.seal();
    LoadRig r;
    r.card.write("modules/apps/writer.tmod", b.file());
    CHECK(r.run("writer") == ModuleLoader::State::Done);
    NEED(r.set.count() == 1u);
    CHECK(r.set.at(0)->header.reserved2[0] == (O3M_CAP_READ_PROJECT | O3M_CAP_EXPORT_FILES));
  }
  for (const unsigned minor : {3u, 4u, 5u, 6u}) {  // ... and a reserved field in a file of 1.3 .. 1.6: quarantined (the file is wrong, the OS is not old)
    Blob b = blob_app(1, static_cast<std::uint8_t>(minor));
    b.h.reserved2[0] = O3M_CAP_EXPORT_FILES;
    b.seal();
    LoadRig r;
    r.card.write("modules/apps/liar.tmod", b.file());
    CHECK(r.run("liar") == ModuleLoader::State::Failed);
    CHECK(r.loader.status() == Status::BadReserved);
    CHECK(r.set.quarantined("liar"));
  }
  {
    Blob b = blob_app(1, 3);  // ... and is a reserved field in a file of 1.3: quarantined (the file is wrong)
    b.h.reserved2[0] = O3M_CAP_AUDIO_TAP;
    b.seal();
    LoadRig r;
    r.card.write("modules/apps/liar.tmod", b.file());
    CHECK(r.run("liar") == ModuleLoader::State::Failed);
    CHECK(r.loader.status() == Status::BadReserved);
    CHECK(r.set.quarantined("liar"));
  }
  {
    Blob b = blob_app(1, 3);  // a bit nobody published
    b.h.reserved2[0] = 64u;
    b.seal();
    LoadRig r;
    r.card.write("modules/apps/later.tmod", b.file());
    CHECK(r.run("later") == ModuleLoader::State::Failed);
    CHECK(r.loader.status() == Status::BadReserved);
    CHECK(r.set.quarantined("later"));
  }
  {
    Blob b = good_blob();  // an instrument of ABI 1.3 may not ask
    b.h.abi_minor = 3;
    b.h.reserved2[0] = O3M_CAP_MIDI_IN;
    b.seal();
    LoadRig r;
    r.card.write("modules/instruments/greedy.tmod", b.file());
    CHECK(r.run("greedy") == ModuleLoader::State::Failed);
    CHECK(r.loader.status() == Status::BadReserved);
  }
  {
    ModuleSet set;  // a module compiled into the process: the same rules for the mask it is registered with
    static const o3m_ui ui = {sizeof(o3m_ui), nullptr, nullptr, [](void*, std::uint32_t, o3m_canvas*) {}, nullptr, nullptr};
    static const o3m_module app = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_APP, sizeof(o3m_module), "n", 16, 0, nullptr,
                                   [](void*, const o3m_host*) { return 0; }, nullptr, nullptr, nullptr, nullptr, [](void*) {}, &ui, 1};
    CHECK(set.add_native("n", app, O3M_CAPS_ABI_1_3));
    CHECK(set.at(0)->header.reserved2[0] == O3M_CAPS_ABI_1_3);
    CHECK(set.add_native("m", app, O3M_CAP_AUDIO_TAP));  // the descriptor above is ABI 1.5 (O3M_ABI_MINOR)
    CHECK(set.add_native("x", app, O3M_CAP_EXPORT_FILES));  // (OS-H4) the descriptor above is ABI 1.7: this OS grants it
    CHECK(!set.add_native("y", app, O3M_CAP_MIDI_OUT));  // this OS does not grant it
    static const o3m_module app16 = {O3M_ABI_MAJOR, 6, O3M_KIND_APP, O3M_MODULE_SIZE(6), "n6", 16, 0, nullptr,
                                     [](void*, const o3m_host*) { return 0; }, nullptr, nullptr, nullptr, nullptr, [](void*) {}, &ui, 1};
    CHECK(!set.add_native("z", app16, O3M_CAP_EXPORT_FILES));  // an app of ABI 1.6 cannot hold it
  }
}

TEST_CASE("module abi: the SDK constants the packer and the generator rely on") {
  CHECK(O3M_ABI_MAJOR == 1);
  CHECK(O3M_ABI_MINOR == 13);  // (OS-MODRATE S2; OS-INSFX-ABI2 A1; OS-FLASHDIET I1 was 11)
  CHECK(O3M_VIEWS_MAX == 8u);
  CHECK(O3M_PARAM_UNIT(O3M_PARAM_WITH_UNIT(O3M_UNIT_HZ) | O3M_PARAM_LOG) == O3M_UNIT_HZ);
  CHECK((O3M_PARAM_WITH_UNIT(O3M_UNIT_COUNT - 1u) & ~O3M_PARAM_UNIT_MASK) == 0u);
}

TEST_CASE("module view: text is cut by whole characters at the canvas edge and dropped when its row is not inside") {
  struct Local {
    static void draw(void*, std::uint32_t, o3m_canvas* c) {
      const o3m_draw_api* a = draw_api();
      a->text(c, 88, 4, "ABC", O3M_FACE_DATA, O3M_COLOR_TEXT);   // 6 px a character on a 100 px canvas: AB fit (88..100), C does not
      a->text(c, 95, 4, "ABC", O3M_FACE_DATA, O3M_COLOR_TEXT);   // not one whole character
      a->text(c, 0, 75, "ROW", O3M_FACE_DATA, O3M_COLOR_TEXT);   // 12 px high at y 75 on an 80 px canvas
      a->text(c, 0, -1, "ROW", O3M_FACE_DATA, O3M_COLOR_TEXT);
      a->text(c, -13, 0, "WXYZ", O3M_FACE_DATA, O3M_COLOR_TEXT); // W, X wholly left (-13..-1), Y starts at -1: also cut, Z whole from 5
      a->text(c, 0, 0, "0123456789ABCDEFGHIJ0123456789ABCDEFGHIJ0123456789ABCDEFGHIJ0123456789ABCDEFGHIJ", O3M_FACE_DATA, O3M_COLOR_TEXT);  // more than a call keeps: cut at 64
    }
  };
  static const o3m_ui ui = {sizeof(o3m_ui), nullptr, nullptr, Local::draw, nullptr, nullptr};
  const o3m_module mod = {O3M_ABI_MAJOR, O3M_ABI_MINOR, O3M_KIND_INSTRUMENT, sizeof(o3m_module), "o", 16, 0, nullptr,
                          fake_init, fake_render, fake_set_param, fake_note_on, fake_note_off, fake_shutdown, &ui, 1};
  Rig r;
  NEED(r.host.draw(mod, nullptr, 1, 0, 100, 80, nullptr, 0) == ViewStatus::Ok);
  r.host.replay(r.sink);
  NEED(r.sink.ev.size() == 3u);
  CHECK((r.sink.ev[0].text == "AB" && r.sink.ev[0].x == 88));
  CHECK((r.sink.ev[1].text == "Z" && r.sink.ev[1].x == 5));
  // the long one: 64 characters kept, of which 16 whole ones fit in 100 px (6 px each)
  CHECK((r.sink.ev[2].text.size() == 16u && r.sink.ev[2].x == 0));
  CHECK(r.sink.ev[2].text == "0123456789ABCDEF");
}
