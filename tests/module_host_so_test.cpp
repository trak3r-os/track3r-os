// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-012a: the modules built for the HOST from the same source as the .tmod files (modules/*, CMake o3m_<name>_host): loaded with dlopen, their
// descriptor held to what the loader holds a file's to (ModuleSet::add_native), and driven through the ABI (o3m_host, init / set_param / note_on /
// render / note_off). The TB-303's glue must be nothing but the voice: bit for bit what tb303_dsp.h gives when called directly. Host only.
#include <doctest/doctest.h>

#define NEED(cond)                                  \
  do {                                              \
    const bool need_ok_ = static_cast<bool>(cond);  \
    CHECK(need_ok_);                                \
    if (!need_ok_) {                                \
      return;                                       \
    }                                               \
  } while (0)

#include <o3os/module/loader.h>

#include "../modules/tb303/tb303_dsp.h"

#include <dlfcn.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace o3os::module;

float x_exp2(float x) { return std::exp2(x); }
float x_log2(float x) { return std::log2(x); }
float x_sin(float x) { return std::sin(x); }
float x_cos(float x) { return std::cos(x); }
void x_log(const char*) {}

o3m_host make_host(float rate) {
  o3m_host h{};
  h.abi_major = O3M_ABI_MAJOR;
  h.abi_minor = O3M_ABI_MINOR;
  h.size = sizeof h;
  h.sample_rate = rate;
  h.log = x_log;
  h.exp2_f = x_exp2;
  h.log2_f = x_log2;
  h.sin_f = x_sin;
  h.cos_f = x_cos;
  h.draw = nullptr;
  return h;
}

struct Loaded {
  void* handle = nullptr;
  const o3m_module* entry = nullptr;
  ~Loaded() {
    if (handle != nullptr) {
      dlclose(handle);
    }
  }
};

bool open_module(const char* path, Loaded& out) {
  out.handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (out.handle == nullptr) {
    return false;
  }
  out.entry = static_cast<const o3m_module*>(dlsym(out.handle, "o3m_entry"));
  return out.entry != nullptr;
}

struct State {
  alignas(16) unsigned char mem[1024] = {};
};

}  // namespace

TEST_CASE("module host shared objects: the descriptors are valid and say what the sources say") {
  Loaded sine, tb;
  NEED(open_module(O3M_SINE_HOST_SO, sine));
  NEED(open_module(O3M_TB303_HOST_SO, tb));
  ModuleSet set;
  CHECK(set.add_native("sine", *sine.entry));
  CHECK(set.add_native("tb303", *tb.entry));
  CHECK(set.count() == 2);
  CHECK(std::string(tb.entry->name) == "TB-303");
  CHECK(tb.entry->kind == O3M_KIND_INSTRUMENT);
  CHECK(tb.entry->param_count == 14);  // nine, and the five OS-SNDMOD S3 appended (ids 9..13)
  CHECK(tb.entry->state_bytes == sizeof(tb303::Voice));
  CHECK(tb.entry->state_bytes <= sizeof(State::mem));
  for (std::uint32_t i = 0; i < tb.entry->param_count; ++i) {
    const o3m_param& p = tb.entry->params[i];
    CAPTURE(std::string(p.name));
    CHECK(std::strlen(p.name) < O3M_PARAM_NAME_MAX);
    CHECK(p.min <= p.def);
    CHECK(p.def <= p.max);
  }
  CHECK(std::string(sine.entry->name) == "Sine");
  // a parameter blob is one float per parameter in this order (the OS keeps it verbatim)
  CHECK(std::string(tb.entry->params[tb303::kCutoff].name) == "Cutoff");
  CHECK(tb.entry->params[tb303::kLegato].flags == O3M_PARAM_BOOL);
  // the descriptor of a module of a newer ABI or the wrong size is refused
  o3m_module bad = *tb.entry;
  bad.abi_minor = O3M_ABI_MINOR + 1;
  CHECK(!set.add_native("newer", bad));
  bad = *tb.entry;
  bad.struct_size = 12;
  CHECK(!set.add_native("short", bad));
  bad = *tb.entry;
  bad.render = nullptr;
  CHECK(!set.add_native("norender", bad));
  // the TB-303 declares two views (OS-012b) with the draw table's one entry point
  CHECK(tb.entry->view_count == 2u);
  CHECK((tb.entry->ui != nullptr && tb.entry->ui->view_draw != nullptr && tb.entry->ui->struct_size == O3M_UI_SIZE(13)));
  CHECK(sine.entry->view_count == 0u);
  CHECK(sine.entry->ui == nullptr);
  bad = *tb.entry;
  bad.ui = nullptr;  // views without a table
  CHECK(!set.add_native("withui", bad));
  bad = *tb.entry;
  bad.view_count = 0;  // a table without views
  CHECK(!set.add_native("views", bad));
  bad = *tb.entry;
  bad.view_count = O3M_VIEWS_MAX + 1;
  CHECK(!set.add_native("manyviews", bad));
  bad = *tb.entry;
  bad.kind = O3M_KIND_IMPORTER;  // importers are not loaded by this OS
  CHECK(!set.add_native("importer", bad));
  CHECK(!set.add_native("tb303", *tb.entry));  // a name is taken once
}

TEST_CASE("module host shared objects: the TB-303 through the ABI is the voice, bit for bit") {
  Loaded tb;
  NEED(open_module(O3M_TB303_HOST_SO, tb));
  const o3m_host host = make_host(44117.647f);
  State state;
  NEED(tb.entry->init(state.mem, &host) == 0);
  tb303::Voice direct;
  std::memset(&direct, 0, sizeof direct);
  const tb303::Services sv = {x_exp2, x_log2, x_sin, x_cos};
  tb303::voice_init(direct, sv, host.sample_rate);
  const float values[] = {0.4f, 700.0f, 75.0f, 55.0f, 350.0f, 80.0f, 90.0f, -9.0f, 1.0f};
  for (std::uint32_t i = 0; i < 9; ++i) {
    tb.entry->set_param(state.mem, i, values[i]);
    tb303::voice_set_param(direct, i, values[i]);
  }
  float a[64], b[64];
  bool same = true;
  for (int blk = 0; blk < 600; ++blk) {
    if (blk == 0 || blk == 150 || blk == 300) {
      const std::uint32_t note = blk == 0 ? 38u : (blk == 150 ? 50u : 52u);
      const std::uint32_t vel = blk == 150 ? 127u : 80u;
      tb.entry->note_on(state.mem, note, vel, blk == 0 ? 0u : O3M_NOTE_LEGATO);
      tb303::voice_note_on(direct, note, vel, blk != 0);
    }
    if (blk == 450) {
      tb.entry->note_off(state.mem, 52);
      tb303::voice_note_off(direct);
    }
    const std::uint32_t ra = tb.entry->render(state.mem, nullptr, a, 64);
    const std::uint32_t rb = tb303::voice_render(direct, b, 64);
    same = same && ra == rb && std::memcmp(a, b, sizeof a) == 0;
  }
  CHECK(same);
  tb.entry->shutdown(state.mem);
}

TEST_CASE("module host shared objects: the sine example sounds at its note's pitch and ends after its release") {
  Loaded sine;
  NEED(open_module(O3M_SINE_HOST_SO, sine));
  const o3m_host host = make_host(44100.0f);
  State state;
  NEED(sine.entry->init(state.mem, &host) == 0);
  sine.entry->set_param(state.mem, 0, 1.0f);
  sine.entry->set_param(state.mem, 1, 20.0f);
  sine.entry->note_on(state.mem, 69, 100, 0);  // A4, 440 Hz
  std::vector<float> out(44100);
  for (std::size_t i = 0; i < out.size(); i += 64) {
    CHECK(sine.entry->render(state.mem, nullptr, out.data() + i, static_cast<std::uint32_t>(i + 64 <= out.size() ? 64 : out.size() - i)) == 1);
  }
  int crossings = 0;
  for (std::size_t i = 4410; i + 1 < out.size(); ++i) {  // the second half-second on
    crossings += (out[i] < 0.0f && out[i + 1] >= 0.0f) ? 1 : 0;
  }
  const double hz = crossings / ((static_cast<double>(out.size()) - 4410.0) / 44100.0);
  CHECK(hz > 437.5);  // a count of crossings in 0.9 s is good to one crossing: 1.1 Hz
  CHECK(hz < 442.5);
  float peak = 0;
  for (float x : out) {
    peak = std::fmax(peak, std::fabs(x));
  }
  CHECK(peak > 0.95f);
  CHECK(peak <= 1.001f);
  sine.entry->note_off(state.mem, 69);
  std::uint32_t active = 1;
  int blocks = 0;
  float tail[64];
  while (active != 0 && blocks < 100) {
    active = sine.entry->render(state.mem, nullptr, tail, 64);
    ++blocks;
  }
  CHECK(active == 0);
  CHECK(blocks <= 20);  // 20 ms of release is 14 blocks
  sine.entry->shutdown(state.mem);
}
