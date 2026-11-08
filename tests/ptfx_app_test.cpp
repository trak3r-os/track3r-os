// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-CEPTFX S3b: the plugin run on the App (App::run_ptfx, the finish_edit branch, report_jobs' "PLUGIN ..." strip rule, App::cancel_ptfx; docs/specs/OS-CEPTFX.md 3.5) with a host
// stand-in plugin through PtfxJob::Plugin::call: the commit is one undo unit and one history step and F8 restores the bytes, a selection runs only on its range, a result of another
// length resets the selection, a sample that changed meanwhile is left alone, a second job is refused, a hung or crashed plugin leaves the bytes and says why, a cancel keeps the audio.
// The job's own sequence is ptfx_job_test.cpp.
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3os/app/app.h>
#include <o3os/app/play_mode.h>
#include <o3os/app/ptfx_job.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/screens/ids.h>
#include <o3os/audio/voices.h>
#include <o3os/core/action.h>
#include <o3os/core/input_event.h>
#include <o3os/model/editor.h>
#include <o3os/module/ptfx_host_table.h>
#include <o3os/module/ptfx_quarantine.h>
#include <o3os/platform/host/fakes.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace o3os;
using app::Button;
using app::PtfxJob;
using app::ScreenId;
using hal::code_gate::GateResult;

namespace {

alignas(8) std::uint8_t g_ring[256 * 1024];
constexpr std::uint32_t kFrames = 20000;
constexpr unsigned kSlot = 2;  // instrument 3

constexpr std::int16_t kMade = 7000;  // what the stand-in writes where it has no input
constexpr unsigned kEmpty = 5;        // a slot with no instrument

constexpr std::uintptr_t kInit = 0x100;
constexpr std::uintptr_t kSet = 0x200;
constexpr std::uintptr_t kProc = 0x300;

// The stand-in: it doubles the body. `produce_limit` frames in all, then it returns 0 (a shorter result); `fail_proc` makes the first process_block answer with that gate verdict.
struct Fake {
  std::uint32_t produce_limit = 0xFFFFFFFFu;
  GateResult fail_proc = GateResult::Ok;
  unsigned proc_calls = 0;
  std::uint32_t want_prepend = 0;  // set_extra_samples in the replay (Add Silence)
  std::uint32_t want_append = 0;
};
Fake g;

GateResult fake_call(const PtfxJob::Plugin&, std::uintptr_t entry, std::uintptr_t r0, std::uintptr_t r1, std::uintptr_t r2, std::uintptr_t, float, std::int32_t* ret) noexcept {
  *ret = 0;
  if (entry == kSet && r0 == 3) {
    module::PtfxHostTable::set_extra_samples(g.want_prepend, g.want_append);  // inside the replay: latched
  }
  if (entry != kProc) {
    return GateResult::Ok;
  }
  ++g.proc_calls;
  if (g.fail_proc != GateResult::Ok) {
    return g.fail_proc;
  }
  const auto* in = reinterpret_cast<const std::int16_t* const*>(r0);
  auto* out = reinterpret_cast<std::int16_t* const*>(r1);
  const std::uint32_t n = static_cast<std::uint32_t>(r2);
  const std::uint32_t make = n < g.produce_limit ? n : g.produce_limit;
  for (std::uint32_t i = 0; i < make; ++i) {
    out[0][i] = in[0] != nullptr ? static_cast<std::int16_t>(in[0][i] * 2) : kMade;  // NULL in: a generator, or the silence around the body
  }
  g.produce_limit -= make;
  *ret = static_cast<std::int32_t>(make);
  return GateResult::Ok;
}

std::int16_t sample_at(std::uint32_t i) { return static_cast<std::int16_t>(std::lround(8000.0 * std::sin(0.03 * i + 0.4))); }

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<audio::VoicePool> voices = std::make_unique<audio::VoicePool>();
  std::unique_ptr<model::ModelArena> arena = std::make_unique<model::ModelArena>();
  std::vector<std::int16_t> memory;
  std::vector<std::int16_t> peaks;
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  std::unique_ptr<app::SampleBankMem> mem = std::make_unique<app::SampleBankMem>();
  app::SampleBank bank;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);
  std::vector<std::int16_t> original;

  module::PtfxInfo info{};
  module::PtfxHostTableStorage store{};
  module::PtfxHostTable host{store.bytes};
  alignas(4) std::uint8_t scratch[4096] = {};
  module::PtfxQuarantine quarantine;
  PtfxJob::Plugin plugin;

  Rig() : memory(200000), peaks(20000) {
    g = Fake{};
    arena->project.reset();
    bank.init(*voices, *arena, *mem, memory.data(), 200000, peaks.data(), peaks.size());
    model::Instrument in;
    model::init_instrument(in, model::InstrumentKind::Sample);
    std::strcpy(in.name, "LOOP");
    NEED(arena->project.set_instrument(kSlot, in) == model::Status::Ok);
    load(kFrames, 0);
    original.assign(bank.pcm(kSlot).data, bank.pcm(kSlot).data + kFrames);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(arena->project, journal);
    app->attach_document(editor.get());
    app->attach_samples(&bank);
    app->invoke(core::action_id("instrument.select"), static_cast<core::ActionArg>(kSlot + 1));
    go(ScreenId::SampleEdit);
    passes(30);
    expire();

    info.status = module::PtfxStatus::Ok;
    std::strcpy(info.name, "Gain");
    info.num_params = 1;
    info.scratch_size = 1024;
    plugin.info = &info;
    plugin.entries = {kInit, kSet, kProc};
    plugin.host = &host;
    plugin.host_table = reinterpret_cast<std::uintptr_t>(store.bytes);
    plugin.scratch = scratch;
    plugin.scratch_cap = sizeof scratch;
    plugin.id = module::ptfx_plugin_id("gain.ptfx");
    plugin.quarantine = &quarantine;
    plugin.call = &fake_call;
  }
  // A new audio for the slot (what a load does: the version moves).
  void load(std::uint32_t frames, std::int16_t bias) {
    std::uint32_t cap = 0;
    std::int16_t* p = bank.reserve(cap);
    NEED(p != nullptr);
    for (std::uint32_t i = 0; i < frames; ++i) {
      p[i] = static_cast<std::int16_t>(sample_at(i) + bias);
    }
    NEED(bank.commit_load(kSlot, p, frames));
    bank.rebind_now(kSlot);
  }
  void pass(unsigned ms = 16) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  void passes(unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
      pass();
    }
  }
  void go(ScreenId id) {
    (void)app->invoke(core::action_id("nav.screen"), static_cast<core::ActionArg>(id));
    pass();
  }
  void press(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true)); }
  void release(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false)); }
  void tap(Button b) {
    press(b);
    release(b);
    pass();
  }
  void invoke(const char* name, core::ActionArg arg = 0) {
    (void)app->invoke(core::action_id(name), arg);
    pass();
  }
  void expire() {
    for (unsigned i = 0; i < 10 && !std::string(app->message()).empty(); ++i) {
      clock.advance(3000000);
      app->run_once();
    }
  }
  // Starts a run and lets it end; the strip is left as the run said it.
  const char* start() { return app->run_ptfx(plugin); }
  void finish() { passes(60); }
  const app::SampleEditState& st() const { return app->state().sedit; }
  app::SampleBank::Pcm pcm() const { return bank.pcm(kSlot); }
  bool same_as_original() const {
    return pcm().frames == kFrames && std::memcmp(pcm().data, original.data(), kFrames * sizeof(std::int16_t)) == 0;
  }
  // The Sample edit screen on the empty slot.
  void open_empty() {
    go(ScreenId::Pattern);
    (void)app->invoke(core::action_id("instrument.select"), static_cast<core::ActionArg>(kEmpty + 1));
    go(ScreenId::SampleEdit);
    passes(30);
    expire();
  }
  void generator(std::uint16_t more_flags = 0) { info.flags = static_cast<std::uint16_t>(module::kPtfxFlagGenerator | more_flags); }
  void select(std::uint32_t clicks_a, std::uint32_t clicks_b) {
    tap(Button::Left);
    invoke("sedit.move", static_cast<core::ActionArg>(clicks_a));
    tap(Button::Right);
    invoke("sedit.move", -static_cast<core::ActionArg>(clicks_b));
  }
};

}  // namespace

TEST_CASE("ptfx app: a run of the whole sample is one undo step, and F8 restores the bytes") {
  Rig rig;
  CHECK(rig.bank.undo_depth(kSlot) == 0u);
  CHECK(rig.start() == nullptr);
  rig.finish();
  NEED(rig.pcm().frames == kFrames);
  CHECK(rig.bank.undo_depth(kSlot) == 1u);
  bool all = true;
  for (std::uint32_t i = 0; i < kFrames; ++i) {
    all = all && rig.pcm().data[i] == rig.original[i] * 2;
  }
  CHECK(all);
  CHECK(!rig.app->ptfx_busy());
  rig.expire();
  rig.tap(Button::F8);
  CHECK(rig.bank.undo_depth(kSlot) == 0u);
  CHECK(rig.same_as_original());
}

TEST_CASE("ptfx app: a selection is the plugin's body; the audio around it is kept and the selection stays") {
  Rig rig;
  rig.select(100, 300);
  const std::uint32_t a = rig.st().a;
  const std::uint32_t b = rig.st().b;
  NEED(a > 0u);
  NEED(b > a);
  NEED(b < kFrames);
  CHECK(rig.start() == nullptr);
  rig.finish();
  NEED(rig.pcm().frames == kFrames);
  bool ok = true;
  for (std::uint32_t i = 0; i < kFrames; ++i) {
    const std::int16_t want = (i >= a && i < b) ? static_cast<std::int16_t>(rig.original[i] * 2) : rig.original[i];
    ok = ok && rig.pcm().data[i] == want;
  }
  CHECK(ok);
  CHECK(rig.bank.undo_depth(kSlot) == 1u);
  CHECK(rig.st().a == a);  // the length is the same: the selection means what it did
  CHECK(rig.st().b == b);
}

TEST_CASE("ptfx app: a result of another length is the sample and the selection of the old length is reset") {
  Rig rig;
  rig.select(100, 300);
  const std::uint32_t a = rig.st().a;
  const std::uint32_t b = rig.st().b;
  NEED(b > a);
  NEED(b < kFrames);
  g.produce_limit = 5000;
  // The selection is a range of the old length; a plugin that returns fewer frames than the body makes the result shorter: the audio before, the 5000 it made, the audio after.
  CHECK(rig.start() == nullptr);
  rig.finish();
  CHECK(rig.pcm().frames == a + 5000u + (kFrames - b));
  CHECK(rig.bank.undo_depth(kSlot) == 1u);
  CHECK(rig.st().a == 0u);
  CHECK(rig.st().b == 0u);
  rig.expire();
  rig.tap(Button::F8);
  CHECK(rig.same_as_original());
}

TEST_CASE("ptfx app: a second job is refused while one runs, and a run needs the Sample edit screen with a sample") {
  Rig rig;
  CHECK(rig.start() == nullptr);
  CHECK(rig.app->ptfx_busy());
  CHECK(std::string(rig.start()) == "ANOTHER JOB IS RUNNING");
  rig.finish();
  CHECK(!rig.app->ptfx_busy());
  CHECK(rig.bank.undo_depth(kSlot) == 1u);
  rig.go(ScreenId::Pattern);
  CHECK(std::string(rig.start()) == "OPEN A SAMPLE IN SAMPLE EDIT");
  CHECK(!rig.app->ptfx_busy());
}

TEST_CASE("ptfx app: ptfx_refusal answers what run_ptfx would, and ptfx_busy guards the shared info during a run") {
  Rig rig;
  CHECK(rig.app->ptfx_refusal() == nullptr);
  CHECK(!rig.app->ptfx_busy());
  CHECK(rig.start() == nullptr);
  CHECK(rig.app->ptfx_busy());  // platform/mk66 app_main.cpp ptfx_info_line returns before it touches the shared info while this holds
  CHECK(std::string(rig.app->ptfx_refusal()) == "ANOTHER JOB IS RUNNING");
  CHECK(rig.info.num_params == 1u);  // (the job's Params phase reads it; nothing here changes it)
  rig.finish();
  CHECK(!rig.app->ptfx_busy());
  CHECK(rig.app->ptfx_refusal() == nullptr);
  rig.go(ScreenId::Pattern);
  CHECK(std::string(rig.app->ptfx_refusal()) == "OPEN A SAMPLE IN SAMPLE EDIT");
}

TEST_CASE("ptfx app: the plugin's host table gets the instrument's sample rate") {
  Rig rig;
  model::Instrument in = *rig.arena->project.instrument(kSlot);
  in.sample_rate = 22050;
  NEED(rig.arena->project.set_instrument(kSlot, in) == model::Status::Ok);
  CHECK(rig.start() == nullptr);
  rig.finish();
  std::uint32_t rate = 0;
  std::memcpy(&rate, rig.store.bytes, sizeof rate);  // the host table's first word is sample_rate
  CHECK(rate == 22050u);
}

TEST_CASE("ptfx app: leaving Sample edit while the plugin runs ends with THE EDIT COULD NOT BE KEPT and the bytes as they were") {
  Rig rig;
  CHECK(rig.start() == nullptr);
  rig.go(ScreenId::Pattern);
  rig.finish();
  CHECK(std::string(rig.app->message()) == "THE EDIT COULD NOT BE KEPT");
  CHECK(rig.bank.undo_depth(kSlot) == 0u);
  CHECK(rig.same_as_original());
  CHECK(!rig.app->ptfx_busy());
}

TEST_CASE("ptfx app: a sample that changed while the plugin ran is left as the load made it (version mismatch)") {
  Rig rig;
  CHECK(rig.start() == nullptr);
  rig.load(kFrames, 7);  // another audio takes the slot: the version moves
  rig.finish();
  CHECK(rig.bank.undo_depth(kSlot) == 0u);
  NEED(rig.pcm().frames == kFrames);
  CHECK(rig.pcm().data[10] == sample_at(10) + 7);  // not the doubled result
  CHECK(!rig.app->ptfx_busy());
}

TEST_CASE("ptfx app: a hung plugin times out, is quarantined and leaves the bytes; the strip is the plugin's text") {
  Rig rig;
  g.fail_proc = GateResult::Timeout;
  CHECK(rig.start() == nullptr);
  rig.finish();
  CHECK(std::string(rig.app->message()) == "PLUGIN TIMED OUT: QUARANTINED");
  CHECK(rig.quarantine.contains(rig.plugin.id));
  CHECK(rig.same_as_original());
  CHECK(rig.bank.undo_depth(kSlot) == 0u);
  CHECK(!rig.app->ptfx_busy());
  rig.expire();
  CHECK(std::string(rig.start()) == "PLUGIN QUARANTINED");  // it stays out until it is released
}

TEST_CASE("ptfx app: a crashing plugin is contained, the strip warns, the bytes are as they were") {
  Rig rig;
  g.fail_proc = GateResult::Fault;
  CHECK(rig.start() == nullptr);
  rig.finish();
  CHECK(std::string(rig.app->message()) == "PLUGIN CRASHED: SAVE, THEN RESTART");
  CHECK(rig.same_as_original());
  CHECK(rig.bank.undo_depth(kSlot) == 0u);
  CHECK(!rig.app->ptfx_busy());
}

TEST_CASE("ptfx app: cancel_ptfx stops the run and keeps the audio") {
  Rig rig;
  CHECK(rig.start() == nullptr);
  rig.app->cancel_ptfx();
  rig.finish();
  CHECK(rig.same_as_original());
  CHECK(rig.bank.undo_depth(kSlot) == 0u);
  CHECK(!rig.app->ptfx_busy());
  CHECK(rig.start() == nullptr);  // the slot of the runner is free again
  rig.finish();
  CHECK(rig.bank.undo_depth(kSlot) == 1u);
}

// OS-CEPTFX S4c: generators and Add Silence on the App (the simulator has no stand-in for them: QA-SED-035..038 are these tests).
TEST_CASE("ptfx app: a generator on a slot with a sample replaces the whole sample as one undo step and F8 restores it") {
  Rig rig;
  rig.generator();
  rig.select(100, 300);  // the selection is ignored
  CHECK(rig.app->ptfx_refusal(true) == nullptr);
  CHECK(rig.start() == nullptr);
  rig.finish();
  NEED(rig.pcm().frames == app::kPtfxGeneratorFrames);
  CHECK(rig.pcm().data[0] == kMade);
  CHECK(rig.pcm().data[app::kPtfxGeneratorFrames - 1] == kMade);
  CHECK(rig.bank.undo_depth(kSlot) == 1u);
  CHECK(rig.st().a == 0u);
  CHECK(rig.st().b == 0u);
  CHECK(!rig.app->ptfx_busy());
  rig.expire();
  rig.tap(Button::F8);
  CHECK(rig.bank.undo_depth(kSlot) == 0u);
  CHECK(rig.same_as_original());
}

TEST_CASE("ptfx app: a generator on an empty slot goes in like a loaded sample: Gen NN, no history step") {
  Rig rig;
  rig.open_empty();
  CHECK(rig.bank.pcm(kEmpty).frames == 0u);
  CHECK(std::string(rig.app->ptfx_refusal(false)) == "OPEN A SAMPLE IN SAMPLE EDIT");  // an effect still needs a sample
  CHECK(rig.app->ptfx_refusal(true) == nullptr);
  rig.generator();
  g.produce_limit = 12345;  // it stops early: trimmed to what it made
  CHECK(rig.start() == nullptr);
  rig.finish();
  NEED(rig.bank.pcm(kEmpty).frames == 12345u);
  CHECK(rig.bank.pcm(kEmpty).data[0] == kMade);
  CHECK(rig.bank.pcm(kEmpty).data[12344] == kMade);
  CHECK(rig.bank.undo_depth(kEmpty) == 0u);
  const model::Instrument* in = rig.arena->project.instrument(kEmpty);
  NEED(in != nullptr);
  CHECK(in->kind == model::InstrumentKind::Sample);
  CHECK(std::string(in->name) == "Gen 06");  // the number the screens show: slot 5 is instrument 6
  CHECK(in->sampler.play_mode == 0u);
  CHECK(rig.same_as_original());  // the other slot is untouched
  CHECK(!rig.app->ptfx_busy());
  CHECK(rig.app->ptfx_refusal(true) == nullptr);
  // The take was released: a second generator on the now loaded slot is a replacement with one history step.
  g.produce_limit = 0xFFFFFFFFu;
  rig.expire();
  CHECK(rig.start() == nullptr);
  rig.finish();
  CHECK(rig.bank.pcm(kEmpty).frames == app::kPtfxGeneratorFrames);
  CHECK(rig.bank.undo_depth(kEmpty) == 1u);
}

TEST_CASE("ptfx app: a generator that makes nothing, or is cancelled, leaves the empty slot empty and the take released") {
  Rig rig;
  rig.open_empty();
  rig.generator();
  g.produce_limit = 0;
  CHECK(rig.start() == nullptr);
  rig.finish();
  CHECK(rig.bank.pcm(kEmpty).frames == 0u);
  const model::Instrument* in = rig.arena->project.instrument(kEmpty);
  CHECK((in == nullptr || in->kind == model::InstrumentKind::None));
  CHECK(!rig.app->ptfx_busy());
  g.produce_limit = 0xFFFFFFFFu;
  rig.expire();
  CHECK(rig.start() == nullptr);
  rig.app->cancel_ptfx();
  rig.finish();
  CHECK(rig.bank.pcm(kEmpty).frames == 0u);
  CHECK(!rig.app->ptfx_busy());
  std::uint32_t cap = 0;
  CHECK(rig.bank.take_begin(cap) != nullptr);  // not held by the failed runs
  rig.bank.take_end(0);
  rig.expire();
  CHECK(rig.start() == nullptr);  // and a run after them still goes through
  rig.finish();
  CHECK(rig.bank.pcm(kEmpty).frames == app::kPtfxGeneratorFrames);
}

TEST_CASE("ptfx app: a generator is refused on a slot that holds another kind of instrument") {
  Rig rig;
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Midi);
  in.midi_channel = 1;
  NEED(rig.arena->project.set_instrument(kEmpty, in) == model::Status::Ok);
  rig.open_empty();
  CHECK(std::string(rig.app->ptfx_refusal(true)) == "NO SAMPLE INSTRUMENT HERE");
  rig.generator();
  CHECK(std::string(rig.start()) == "NO SAMPLE INSTRUMENT HERE");
  CHECK(rig.bank.pcm(kEmpty).frames == 0u);
}

TEST_CASE("ptfx app: a sample instrument whose audio is missing keeps its name, volume and settings; only the audio and the wavetable fields are set") {
  Rig rig;
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Sample);
  std::strcpy(in.name, "Keep me");
  in.sampler.volume = 77;
  in.sampler.tune = 5;
  NEED(rig.arena->project.set_instrument(kEmpty, in) == model::Status::Ok);
  rig.open_empty();
  rig.generator(module::kPtfxFlagWavetable);
  g.produce_limit = 4096;
  CHECK(rig.app->ptfx_refusal(true) == nullptr);
  CHECK(rig.start() == nullptr);
  rig.finish();
  NEED(rig.bank.pcm(kEmpty).frames == 4096u);
  const model::Instrument* got = rig.arena->project.instrument(kEmpty);
  NEED(got != nullptr);
  CHECK(std::string(got->name) == "Keep me");
  CHECK(got->sampler.volume == 77);
  CHECK(got->sampler.tune == 5);
  CHECK(got->sampler.play_mode == app::kPlayModeWavetable);
}

TEST_CASE("ptfx app: a slot that became another kind of instrument while the generator ran is not overwritten") {
  Rig rig;
  rig.open_empty();
  rig.generator();
  CHECK(rig.start() == nullptr);
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Midi);
  in.midi_channel = 1;
  NEED(rig.arena->project.set_instrument(kEmpty, in) == model::Status::Ok);
  rig.finish();
  CHECK(rig.arena->project.instrument(kEmpty)->kind == model::InstrumentKind::Midi);
  CHECK(rig.bank.pcm(kEmpty).frames == 0u);
  CHECK(!rig.app->ptfx_busy());
  std::uint32_t cap = 0;
  CHECK(rig.bank.take_begin(cap) != nullptr);  // the take was released
  rig.bank.take_end(0);
}

TEST_CASE("ptfx app: the WAVETABLE flag sets the play mode to 6 after the run, on a sample slot and on an empty one") {
  {
    Rig rig;
    rig.generator(module::kPtfxFlagWavetable);
    CHECK(rig.arena->project.instrument(kSlot)->sampler.play_mode == 0u);
    CHECK(rig.start() == nullptr);
    rig.finish();
    CHECK(rig.arena->project.instrument(kSlot)->sampler.play_mode == app::kPlayModeWavetable);
    CHECK(rig.bank.undo_depth(kSlot) == 1u);
  }
  {
    Rig rig;
    rig.open_empty();
    rig.generator(module::kPtfxFlagWavetable);
    g.produce_limit = 4096;
    CHECK(rig.start() == nullptr);
    rig.finish();
    NEED(rig.bank.pcm(kEmpty).frames == 4096u);
    const model::Instrument* in = rig.arena->project.instrument(kEmpty);
    NEED(in != nullptr);
    CHECK(in->sampler.play_mode == app::kPlayModeWavetable);
    CHECK(std::string(in->name) == "Gen 06");
  }
  {
    Rig rig;  // an effect without the flag keeps the play mode
    CHECK(rig.start() == nullptr);
    rig.finish();
    CHECK(rig.arena->project.instrument(kSlot)->sampler.play_mode == 0u);
  }
}

TEST_CASE("ptfx app: a WAVETABLE run is one undo unit with the play mode: F8 restores the mode and the window, Shift + F8 sets them again") {
  Rig rig;
  model::Instrument in = *rig.arena->project.instrument(kSlot);
  in.sampler.wavetable_window_size = 0;  // the instrument's own window (a size the fit changes)
  in.sampler.wavetable_window = 7;
  NEED(rig.arena->project.set_instrument(kSlot, in) == model::Status::Ok);
  rig.generator(module::kPtfxFlagWavetable);
  CHECK(rig.start() == nullptr);
  rig.finish();
  const auto sp = [&]() -> const model::SamplerParams& { return rig.arena->project.instrument(kSlot)->sampler; };
  NEED(sp().play_mode == app::kPlayModeWavetable);
  CHECK(sp().sample_type == 1u);
  const std::uint16_t size_after = sp().wavetable_window_size;
  const std::uint32_t pos_after = sp().wavetable_window;
  CHECK(size_after != 0u);
  rig.expire();
  rig.tap(Button::F8);
  CHECK(rig.same_as_original());
  CHECK(sp().play_mode == 0u);
  CHECK(sp().sample_type == 0u);
  CHECK(sp().wavetable_window_size == 0u);
  CHECK(sp().wavetable_window == 7u);
  rig.press(Button::Shift);
  rig.tap(Button::F8);
  rig.release(Button::Shift);
  CHECK(sp().play_mode == app::kPlayModeWavetable);
  CHECK(sp().wavetable_window_size == size_after);
  CHECK(sp().wavetable_window == pos_after);
}

TEST_CASE("ptfx app: Add Silence lengthens the sample, the points follow the frames, and F8 restores both") {
  Rig rig;
  model::Instrument in = *rig.arena->project.instrument(kSlot);
  in.sampler.start = 0;
  in.sampler.loop_start = 16384;
  in.sampler.loop_end = 32768;
  in.sampler.end = 65535;
  NEED(rig.arena->project.set_instrument(kSlot, in) == model::Status::Ok);
  g.want_prepend = 2000;
  g.want_append = 3000;
  CHECK(rig.start() == nullptr);
  rig.finish();
  NEED(rig.pcm().frames == kFrames + 5000u);
  CHECK(rig.pcm().data[0] == kMade);  // the silence the plugin wrote (the stand-in's marker)
  CHECK(rig.pcm().data[2000 + 5] == rig.original[5] * 2);
  CHECK(rig.pcm().data[kFrames + 5000 - 1] == kMade);
  CHECK(rig.bank.undo_depth(kSlot) == 1u);
  const model::SamplerParams& sp = rig.arena->project.instrument(kSlot)->sampler;
  const auto near = [](int got, int want) { return got >= want - 3 && got <= want + 3; };
  CHECK(sp.start == 0u);
  CHECK(near(sp.loop_start, (16384 * 20000 / 65535 + 2000) * 65535 / 25000));  // frame 5000 moves to 7000 of 25000
  CHECK(near(sp.loop_end, (32768 * 20000 / 65535 + 2000) * 65535 / 25000));
  CHECK(sp.end == 65535u);
  rig.expire();
  rig.tap(Button::F8);
  CHECK(rig.same_as_original());
  const model::SamplerParams& back = rig.arena->project.instrument(kSlot)->sampler;
  CHECK(back.loop_start == 16384u);
  CHECK(back.loop_end == 32768u);
  CHECK(back.end == 65535u);
}
