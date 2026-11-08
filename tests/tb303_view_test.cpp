// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-012b: the TB-303's views (modules/tb303/tb303.cpp, ABI 1.1): the filter response and the cutoff envelope, drawn through the OS's draw API from the parameter values the
// page hands over. The host shared object is run through module::ViewHost, and the curves it gives the draw API are compared with a reference of the same formulae in
// std::log10 / std::pow; the budgets of the draw API hold for every parameter value. Host only.
#include <doctest/doctest.h>

#include <o3os/module/loader.h>
#include <o3os/module/view.h>

#include <dlfcn.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "support/need.h"

namespace {

using namespace o3os::module;

struct Curves final : ViewSink {
  struct One {
    std::vector<float> x, y;
    std::uint8_t x_axis, y_axis, flags, color;
    float x_lo, x_hi, y_lo, y_hi;
  };
  std::vector<One> curves;
  void fill(int, int, int, int, std::uint8_t) noexcept override {}
  void text(int, int, const char*, std::uint8_t, std::uint8_t) noexcept override {}
  void polyline(const std::int16_t*, std::uint32_t, std::uint8_t) noexcept override {}
  void curve(const CurveCall& c) noexcept override {
    One o;
    o.y.assign(c.y, c.y + c.n);
    if (c.x != nullptr) {
      o.x.assign(c.x, c.x + c.n);
    }
    o.x_axis = c.x_axis;
    o.y_axis = c.y_axis;
    o.flags = c.flags;
    o.color = c.color;
    o.x_lo = c.x_lo;
    o.x_hi = c.x_hi;
    o.y_lo = c.y_lo;
    o.y_hi = c.y_hi;
    curves.push_back(o);
  }
};

struct Loaded {
  void* handle = nullptr;
  const o3m_module* entry = nullptr;
  ~Loaded() {
    if (handle != nullptr) {
      dlclose(handle);
    }
  }
};

bool open_tb303(Loaded& out) {
  out.handle = dlopen(O3M_TB303_HOST_SO, RTLD_NOW | RTLD_LOCAL);
  if (out.handle == nullptr) {
    return false;
  }
  out.entry = static_cast<const o3m_module*>(dlsym(out.handle, "o3m_entry"));
  return out.entry != nullptr;
}

// params: waveform, cutoff, resonance, env mod, decay, accent, slide, volume, legato
struct Params {
  float v[9] = {0.0f, 1000.0f, 50.0f, 25.0f, 1000.0f, 50.0f, 60.0f, -12.0f, 1.0f};
};

float ref_ladder_db(float f, float fc, float k) {
  const double x = static_cast<double>(f) / static_cast<double>(fc);
  const double x2 = x * x;
  const double re = 1.0 - 6.0 * x2 + x2 * x2 + static_cast<double>(k);
  const double im = 4.0 * x * (1.0 - x2);
  return static_cast<float>(-10.0 * std::log10(re * re + im * im));
}

float ref_k(float res_percent) {
  const double r = static_cast<double>(res_percent) * 0.01;
  return static_cast<float>(3.9 * (1.0 - std::exp(-3.0 * r)) / (1.0 - std::exp(-3.0)));
}

}  // namespace

TEST_CASE("tb303 views: the module declares two views and draws both through the draw API inside the budgets, for every value of its parameters") {
  Loaded tb;
  NEED(open_tb303(tb));
  CHECK(tb.entry->view_count == 2u);
  NEED(tb.entry->ui != nullptr);
  CHECK(tb.entry->abi_minor == 13u);  // ABI 1.13: pitch and the page table (1.8), the RATE flag (1.13, OS-MODRATE); the built-in module carries a version
  ViewHost host;
  Params p;
  // the corners and the middle of the three parameters the views read
  const float cutoffs[] = {314.0f, 1000.0f, 2394.0f};
  const float percents[] = {0.0f, 50.0f, 100.0f};
  const float decays[] = {200.0f, 1000.0f, 2000.0f};
  for (float cutoff : cutoffs) {
    for (float res : percents) {
      for (float env : percents) {
        for (float decay : decays) {
          p.v[1] = cutoff;
          p.v[2] = res;
          p.v[3] = env;
          p.v[4] = decay;
          std::uint32_t page_words = 0;  // the two views share one page's kViewMaxWords (module_ui.cpp): together they must fit, or the second is refused
          for (std::uint32_t view = 0; view < 2u; ++view) {
            CAPTURE(cutoff);
            CAPTURE(res);
            CAPTURE(env);
            CAPTURE(decay);
            CHECK(host.draw(*tb.entry, nullptr, 1, view, 380, 140, p.v, 9) == ViewStatus::Ok);
            CHECK(host.words() <= kViewMaxWords);
            page_words += host.words();
            CHECK(page_words <= kViewMaxWords);
            Curves sink;
            host.replay(sink);
            CHECK(sink.curves.size() == (view == 0u ? 2u : 1u));
            for (const Curves::One& c : sink.curves) {
              for (float y : c.y) {
                CHECK(std::isfinite(y));
                CHECK(y >= c.y_lo - 1e-3f);
                CHECK(y <= c.y_hi + 1e-3f);
              }
            }
          }
        }
      }
    }
  }
  // out-of-range values (a stored blob can hold anything finite) are held to the module's own ranges; a canvas that is not the module's own is ignored
  p.v[1] = -5.0f;
  p.v[2] = 1e9f;
  p.v[3] = -3.0f;
  p.v[4] = 0.0f;
  CHECK(host.draw(*tb.entry, nullptr, 1, 0, 380, 140, p.v, 9) == ViewStatus::Ok);
  CHECK(host.draw(*tb.entry, nullptr, 1, 1, 380, 140, p.v, 9) == ViewStatus::Ok);
  CHECK(host.draw(*tb.entry, nullptr, 1, 0, 380, 140, p.v, 4) == ViewStatus::Ok);  // too few values: it draws nothing
  Curves none;
  host.replay(none);
  CHECK(none.curves.empty());
}

TEST_CASE("tb303: the page table is SOUND from id 0 (eight rows: Waveform to Volume) and MODS from id 8 (Legato Slide and what is appended), within the ABI's limits (OS-SNDMOD S1, T1)") {
  Loaded tb;
  NEED(open_tb303(tb));
  const o3m_module& m = *tb.entry;
  NEED(m.pages != nullptr);
  CHECK(m.page_count == 2u);
  CHECK(std::string(m.pages[0].name) == "SOUND");
  CHECK(m.pages[0].first_param == 0u);
  CHECK(std::string(m.pages[1].name) == "MODS");
  CHECK(m.pages[1].first_param == 8u);
  CHECK(std::string(m.params[7].name) == "Volume");  // the last row of the first page
  CHECK(std::string(m.params[8].name) == "Legato Slide");  // the first row of the second
  CHECK(m.param_count - m.pages[1].first_param <= O3M_PAGE_PARAMS_MAX);
  // OS-SNDMOD S3: ids 9..13 are appended in this order (the ids are persistent once released), with these ranges and defaults; the nine before them are untouched
  CHECK(m.param_count == 14u);
  struct Row {
    const char* name;
    float lo, hi, def;
    std::uint32_t unit;
    bool is_int;
    bool log;
  };
  const Row rows[] = {{"Tune", -100.0f, 100.0f, 0.0f, O3M_UNIT_CENTS, true, false},     {"Transpose", -24.0f, 24.0f, 0.0f, O3M_UNIT_SEMITONES, true, false},
                      {"Acc Thresh", 1.0f, 127.0f, 100.0f, O3M_UNIT_NONE, true, false}, {"Filter Track", 0.0f, 100.0f, 0.0f, O3M_UNIT_PERCENT, false, false},
                      {"Acc Decay", 30.0f, 2000.0f, 200.0f, O3M_UNIT_MS, false, true}};
  for (std::uint32_t k = 0; k < 5u; ++k) {
    CAPTURE(k);
    const o3m_param& p = m.params[9u + k];
    CHECK(std::string(p.name) == rows[k].name);
    CHECK(std::strlen(p.name) <= 12u);  // the caption on the params page
    CHECK(p.min == rows[k].lo);
    CHECK(p.max == rows[k].hi);
    CHECK(p.def == rows[k].def);
    CHECK(O3M_PARAM_UNIT(p.flags) == rows[k].unit);
    CHECK(((p.flags & O3M_PARAM_INT) != 0u) == rows[k].is_int);
    CHECK(((p.flags & O3M_PARAM_LOG) != 0u) == rows[k].log);
    CHECK((p.flags & O3M_PARAM_MOD) == 0u);  // none of them is a modulation target (T6: set_param stays off the matrix)
  }
  // the first nine keep their ids, ranges and defaults
  CHECK(std::string(m.params[0].name) == "Waveform");
  CHECK(std::string(m.params[5].name) == "Accent");
  CHECK(m.params[1].min == 314.0f);
  CHECK(m.params[8].def == 1.0f);
}

TEST_CASE("tb303 views: the filter's curves are the ladder's response at the cutoff and at the envelope's peak") {
  Loaded tb;
  NEED(open_tb303(tb));
  ViewHost host;
  Params p;
  p.v[1] = 1000.0f;  // cutoff
  p.v[2] = 0.0f;     // no resonance: a plain four-pole low-pass
  p.v[3] = 50.0f;
  NEED(host.draw(*tb.entry, nullptr, 1, 0, 380, 140, p.v, 9) == ViewStatus::Ok);
  Curves sink;
  host.replay(sink);
  NEED(sink.curves.size() == 2u);
  const Curves::One& peak = sink.curves[0];  // drawn first, under the other
  const Curves::One& base = sink.curves[1];
  CHECK((base.x_axis == O3M_AXIS_LOG_FREQ && base.y_axis == O3M_AXIS_DB && (base.flags & O3M_CURVE_AXES) != 0u && (base.flags & O3M_CURVE_FILL) != 0u));
  CHECK((peak.color == O3M_COLOR_FX && base.color == O3M_COLOR_INSTR));
  CHECK((base.x_lo == 20.0f && base.x_hi == 16000.0f && base.y_lo == -48.0f && base.y_hi == 24.0f));
  NEED(base.x.size() == base.y.size());
  // every point against the formula in double precision (the polynomial logarithm is good to a hundredth of a dB)
  float worst = 0.0f;
  for (std::size_t i = 0; i < base.x.size(); ++i) {
    const float want = std::clamp(ref_ladder_db(base.x[i], 1000.0f, ref_k(0.0f)), -48.0f, 24.0f);
    worst = std::max(worst, std::fabs(base.y[i] - want));
  }
  CHECK(worst < 0.02f);
  // the shape: flat (0 dB) well below the cutoff, -12 dB at it (four poles at 45 degrees: (sqrt 2)^4 = 4), steep above, never above 0 dB without resonance
  CHECK(std::fabs(base.y.front()) < 0.1f);
  const auto nearest = [&](float hz) {
    std::size_t at = 0;
    for (std::size_t i = 0; i < base.x.size(); ++i) {
      at = std::fabs(std::log(base.x[i] / hz)) < std::fabs(std::log(base.x[at] / hz)) ? i : at;
    }
    return at;
  };
  CHECK(std::fabs(base.y[nearest(1000.0f)] - base.y[nearest(1000.0f)]) < 1e-6f);
  CHECK(base.y[nearest(1000.0f)] < -6.0f);
  CHECK(base.y.back() < -40.0f);
  CHECK(*std::max_element(base.y.begin(), base.y.end()) <= 0.05f);
  // the envelope opens the cutoff up: the peak curve is the same shape further right (its -3 dB point is higher), and more so with more envelope modulation
  const auto knee = [&](const Curves::One& c) {
    for (std::size_t i = 0; i < c.y.size(); ++i) {
      if (c.y[i] < -3.0f) {
        return c.x[i];
      }
    }
    return c.x.back();
  };
  CHECK(knee(peak) > knee(base));
  p.v[3] = 100.0f;
  NEED(host.draw(*tb.entry, nullptr, 1, 0, 380, 140, p.v, 9) == ViewStatus::Ok);
  Curves more;
  host.replay(more);
  NEED(more.curves.size() == 2u);
  CHECK(knee(more.curves[0]) > knee(peak));
  CHECK(knee(more.curves[1]) == knee(base));  // the base curve does not move with it
  // the cutoff knob moves both
  p.v[1] = 2000.0f;
  NEED(host.draw(*tb.entry, nullptr, 1, 0, 380, 140, p.v, 9) == ViewStatus::Ok);
  Curves higher;
  host.replay(higher);
  NEED(higher.curves.size() == 2u);
  CHECK(knee(higher.curves[1]) > knee(base) * 1.5f);
}

TEST_CASE("tb303 views: resonance makes the peak, and the envelope view falls from the opened cutoff to the knob's") {
  Loaded tb;
  NEED(open_tb303(tb));
  ViewHost host;
  Params p;
  p.v[1] = 1000.0f;
  p.v[2] = 100.0f;
  NEED(host.draw(*tb.entry, nullptr, 1, 0, 380, 140, p.v, 9) == ViewStatus::Ok);
  Curves sink;
  host.replay(sink);
  NEED(sink.curves.size() == 2u);
  const Curves::One& base = sink.curves[1];
  CHECK(*std::max_element(base.y.begin(), base.y.end()) > 10.0f);  // k = 3.9: a peak of +20 dB at the cutoff, held to the plot's 24
  // the envelope view: the cutoff against the time, a decay long
  p.v[2] = 50.0f;
  p.v[3] = 80.0f;
  p.v[4] = 500.0f;
  NEED(host.draw(*tb.entry, nullptr, 1, 1, 380, 140, p.v, 9) == ViewStatus::Ok);
  Curves env;
  host.replay(env);
  NEED(env.curves.size() == 1u);
  const Curves::One& c = env.curves[0];
  CHECK((c.x_axis == O3M_AXIS_LINEAR && c.y_axis == O3M_AXIS_LOG_FREQ && c.x.empty()));
  CHECK((c.x_lo == 0.0f && std::fabs(c.x_hi - 1.0f) < 1e-6f));  // two decays: 1 s
  CHECK((c.y_lo == 100.0f && c.y_hi == 12000.0f));
  CHECK(c.y.front() > c.y.back());
  CHECK(c.y.front() > 1000.0f);  // opened above the knob
  for (std::size_t i = 1; i < c.y.size(); ++i) {
    CHECK(c.y[i] <= c.y[i - 1] + 1e-3f);  // monotonic fall
  }
  // the time axis follows the decay
  p.v[4] = 2000.0f;
  NEED(host.draw(*tb.entry, nullptr, 1, 1, 380, 140, p.v, 9) == ViewStatus::Ok);
  Curves slow;
  host.replay(slow);
  NEED(slow.curves.size() == 1u);
  CHECK(std::fabs(slow.curves[0].x_hi - 4.0f) < 1e-5f);
  // no envelope modulation still falls a little (the measured mapping's offset), and ends at the knob's side of the start
  p.v[3] = 0.0f;
  NEED(host.draw(*tb.entry, nullptr, 1, 1, 380, 140, p.v, 9) == ViewStatus::Ok);
  Curves flat;
  host.replay(flat);
  NEED(flat.curves.size() == 1u);
  CHECK(flat.curves[0].y.front() - flat.curves[0].y.back() < c.y.front() - c.y.back());
}
