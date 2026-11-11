// SPDX-License-Identifier: GPL-3.0-or-later
#include "sim_ptfx.h"

#include <o3os/app/app.h>
#include <o3os/app/ptfx_job.h>
#include <o3os/app/ptfx_page.h>
#include <o3os/module/ptfx_format.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace o3os::sim {

namespace {

using module::kPtfxHeaderSize;

void put16(std::vector<std::uint8_t>& v, std::size_t at, std::uint16_t x) {
  v[at] = static_cast<std::uint8_t>(x);
  v[at + 1] = static_cast<std::uint8_t>(x >> 8);
}
void put32(std::vector<std::uint8_t>& v, std::size_t at, std::uint32_t x) {
  put16(v, at, static_cast<std::uint16_t>(x));
  put16(v, at + 2, static_cast<std::uint16_t>(x >> 16));
}
void putf(std::vector<std::uint8_t>& v, std::size_t at, float f) {
  std::uint32_t u;
  std::memcpy(&u, &f, sizeof u);
  put32(v, at, u);
}

struct Param {
  const char* label;
  char type;
  float min, max, def, step;
  const char* unit;   // 'f' and 'd'
  const char* items;  // 'l': the strings, each ended by a NUL (the last one too)
  unsigned count;     // 'l': how many
};

// A header-only plugin: the header of the format (module/ptfx_format.h) and 64 bytes of "code" that never runs (the page reads the header and the list strings; a run is not the simulator's). The
// strings of the list parameters follow the 64 bytes, inside the code size.
std::vector<std::uint8_t> make(const char* name, const char* author, std::uint16_t flags, std::uint16_t version, std::uint32_t scratch, const Param* params, unsigned n) {
  std::vector<std::uint8_t> payload(64, 0);
  std::uint16_t list_at[12] = {};
  for (unsigned i = 0; i < n; ++i) {
    if (params[i].type == 'l') {
      list_at[i] = static_cast<std::uint16_t>(payload.size());
      const char* s = params[i].items;
      for (unsigned k = 0; k < params[i].count; ++k) {
        const std::size_t len = std::strlen(s) + 1u;
        payload.insert(payload.end(), s, s + len);
        s += len;
      }
    }
  }
  std::vector<std::uint8_t> v(kPtfxHeaderSize + payload.size(), 0);
  put32(v, 0, module::kPtfxMagic);
  put16(v, 4, version);
  put16(v, 6, flags);
  std::memcpy(&v[8], name, std::strlen(name));
  std::memcpy(&v[40], author, std::strlen(author));
  v[56] = static_cast<std::uint8_t>(n);
  put32(v, 60, scratch);
  put32(v, 64, kPtfxHeaderSize);
  put32(v, 68, static_cast<std::uint32_t>(payload.size()));
  put32(v, 72, 0);
  put32(v, 76, 0);
  put32(v, 80, 8);
  put32(v, 84, 16);
  for (unsigned i = 0; i < n; ++i) {
    const std::size_t q = 88u + 32u * i;
    std::memcpy(&v[q], params[i].label, std::strlen(params[i].label));
    putf(v, q + 8, params[i].min);
    putf(v, q + 12, params[i].max);
    putf(v, q + 16, params[i].def);
    putf(v, q + 20, params[i].step);
    if (params[i].type == 'l') {
      put16(v, q + 24, list_at[i]);
      v[q + 26] = static_cast<std::uint8_t>(params[i].count);
    } else {
      std::memcpy(&v[q + 24], params[i].unit, std::strlen(params[i].unit));
    }
    v[q + 30] = static_cast<std::uint8_t>(params[i].type);
  }
  std::memcpy(&v[kPtfxHeaderSize], payload.data(), payload.size());
  return v;
}

constexpr std::uint16_t kGen = module::kPtfxFlagGenerator;
constexpr std::uint16_t kSt = module::kPtfxFlagStereo;
constexpr std::uint16_t kWt = module::kPtfxFlagWavetable;
constexpr std::uint16_t kMono = module::kPtfxFlagConvertToMono;

const Param kChorus[] = {
    {"RATE", 'f', 0.1f, 10.0f, 1.5f, 0.1f, "Hz", nullptr, 0},
    {"DEPTH", 'f', 0.0f, 100.0f, 40.0f, 1.0f, "%", nullptr, 0},
    {"VOICES", 'd', 1.0f, 6.0f, 3.0f, 1.0f, "", nullptr, 0},
    {"SHAPE", 'l', 0.0f, 2.0f, 0.0f, 1.0f, "", "SINE\0TRI\0SQUARE\0", 3},
};
const Param kNoise[] = {
    {"LENGTH", 'd', 1.0f, 8.0f, 2.0f, 1.0f, "s", nullptr, 0},
    {"COLOR", 'l', 0.0f, 2.0f, 1.0f, 1.0f, "", "WHITE\0PINK\0BROWN\0", 3},
};
const Param kWave[] = {
    {"HARMONICS", 'd', 1.0f, 64.0f, 8.0f, 1.0f, "", nullptr, 0},
    {"TILT", 'f', -1.0f, 1.0f, 0.0f, 0.05f, "", nullptr, 0},
};
const Param kMonoTool[] = {
    {"AMOUNT", 'f', 0.0f, 100.0f, 100.0f, 1.0f, "%", nullptr, 0},
};
const Param kTwelve[] = {
    {"P1", 'f', 0.0f, 1.0f, 0.5f, 0.01f, "", nullptr, 0},  {"P2", 'f', 0.0f, 1.0f, 0.5f, 0.01f, "", nullptr, 0},
    {"P3", 'f', 0.0f, 1.0f, 0.5f, 0.01f, "", nullptr, 0},  {"P4", 'f', 0.0f, 1.0f, 0.5f, 0.01f, "", nullptr, 0},
    {"P5", 'f', 0.0f, 1.0f, 0.5f, 0.01f, "", nullptr, 0},  {"P6", 'f', 0.0f, 1.0f, 0.5f, 0.01f, "", nullptr, 0},
    {"P7", 'd', 0.0f, 100.0f, 10.0f, 1.0f, "", nullptr, 0}, {"P8", 'd', 0.0f, 100.0f, 10.0f, 1.0f, "", nullptr, 0},
    {"P9", 'd', 0.0f, 100.0f, 10.0f, 1.0f, "", nullptr, 0}, {"P10", 'd', 0.0f, 100.0f, 10.0f, 1.0f, "", nullptr, 0},
    {"P11", 'd', 0.0f, 100.0f, 10.0f, 1.0f, "", nullptr, 0}, {"P12", 'd', 0.0f, 100.0f, 10.0f, 1.0f, "", nullptr, 0},
};

}  // namespace

bool write_demo_ptfx(const std::string& card_root, std::string& error) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::create_directories(fs::path(card_root) / "Effects", ec);
  if (ec) {
    error = "ptfx: the Effects folder could not be made: " + ec.message();
    return false;
  }
  struct File {
    const char* file;
    std::vector<std::uint8_t> bytes;
  };
  const File files[] = {
      {"Chorus.ptfx", make("Chorus", "SIMULATOR", kSt, 1, 8192, kChorus, 4)},
      {"Noise.ptfx", make("Noise Burst", "SIMULATOR", kGen, 1, 0, kNoise, 2)},
      {"Wave Maker.ptfx", make("Wave Maker", "", static_cast<std::uint16_t>(kGen | kWt), 1, 2048, kWave, 2)},
      {"Mono Tool.ptfx", make("Mono Tool", "SIMULATOR", kMono, 1, 0, kMonoTool, 1)},
      {"Twelve.ptfx", make("Twelve Knobs", "SIMULATOR", static_cast<std::uint16_t>(kSt | kMono), 1, 65536, kTwelve, 12)},
      {"Future.ptfx", make("Future Plugin", "SIMULATOR", 0, 2, 0, nullptr, 0)},  // a version this OS does not read: a refused row
      {"A name that is far too long for the catalogue to take at all.ptfx", make("Too Long", "", 0, 1, 0, nullptr, 0)},  // over 47 characters: skipped, counted
  };
  for (const File& f : files) {
    std::ofstream out(fs::path(card_root) / "Effects" / f.file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(f.bytes.data()), static_cast<std::streamsize>(f.bytes.size()));
    if (!out) {
      error = std::string("ptfx: could not write ") + f.file;
      return false;
    }
  }
  return true;
}

namespace {

using hal::code_gate::GateResult;

constexpr std::uintptr_t kStandInInit = 0x100;
constexpr std::uintptr_t kStandInSet = 0x200;
constexpr std::uintptr_t kStandInProcess = 0x300;
float g_stand_in_gain = 1.0f;  // the Mono Tool's AMOUNT in percent over 100: set by set_param, read by process (single-threaded: the simulator's loop)

// The host stand-in of the demo `Mono Tool` (3.7: the sim has no code to load): AMOUNT scales the body, clamped to 16 bits.
GateResult mono_tool_call(const app::PtfxJob::Plugin&, std::uintptr_t entry, std::uintptr_t r0, std::uintptr_t r1, std::uintptr_t r2, std::uintptr_t, float s0,
                          std::int32_t* ret) noexcept {
  *ret = 0;
  if (entry == kStandInSet && r0 == 0) {
    g_stand_in_gain = s0 / 100.0f;
  } else if (entry == kStandInProcess) {
    const auto* in = reinterpret_cast<const std::int16_t* const*>(r0);
    auto* out = reinterpret_cast<std::int16_t* const*>(r1);
    const std::uint32_t n = static_cast<std::uint32_t>(r2);
    for (std::uint32_t i = 0; i < n; ++i) {
      const float v = static_cast<float>(in[0][i]) * g_stand_in_gain;
      out[0][i] = static_cast<std::int16_t>(std::clamp(v, -32768.0f, 32767.0f));
    }
    *ret = static_cast<std::int32_t>(n);
  }
  return GateResult::Ok;
}

}  // namespace

const char* SimPtfxRun::run(void* self, unsigned, const float* values, int slot) noexcept {
  auto* r = static_cast<SimPtfxRun*>(self);
  if (r == nullptr || r->app == nullptr || r->page == nullptr || std::strcmp(r->page->info().name, "Mono Tool") != 0) {
    return "PLUGINS RUN ON THE TRACKER";
  }
  g_stand_in_gain = 1.0f;
  app::PtfxJob::Plugin p;
  p.info = &r->page->info();
  p.entries = {kStandInInit, kStandInSet, kStandInProcess};
  p.host = &r->host;
  p.host_table = reinterpret_cast<std::uintptr_t>(r->store.bytes);
  p.scratch = r->scratch;
  p.scratch_cap = sizeof r->scratch;
  p.id = module::ptfx_plugin_id("Mono Tool.ptfx");
  p.values = values;
  p.quarantine = &r->quarantine;
  p.call = &mono_tool_call;
  return r->app->run_ptfx(p, slot);
}

}  // namespace o3os::sim
