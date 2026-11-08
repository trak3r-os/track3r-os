// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-LIVEP, the app's side: an edit on the Params screen of a SAMPLE instrument reaches the engine's descriptor of the slot. The edit is a ParamChange to the engine's ring
// (the live path, audio_inst_live_test.cpp) and, for every field make_sample_desc reads, a request to the sample bank to rebuild the descriptor when playback is stopped and
// the pads are let go (the bank's handover, sample_bank.h): this file proves that second path with the App, the bank and the voices as the platform wires them.
#include <doctest/doctest.h>

#include "support/need.h"
#include "support/static_slot.h"

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/og_samples.h>
#include <o3os/app/sample_bank.h>
#include <o3os/audio/smoothing.h>
#include <o3os/audio/voices.h>
#include <o3os/audio/wt_contour.h>  // OS-CEWTENV W3a
#include <o3os/core/action.h>
#include <o3os/core/input_event.h>
#include <o3os/core/param.h>
#include <o3os/model/editor.h>
#include <o3os/model/params.h>
#include <o3os/platform/host/fakes.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace o3os;
using app::Button;
namespace ip = model::instrument_param;

namespace {

alignas(8) std::uint8_t g_ring[64 * 1024];

constexpr std::uint32_t kFrames = 20000;
constexpr unsigned kSlot = 2;  // instrument 3

class IdlePreview final : public app::PreviewSink {
 public:
  void preview(unsigned, unsigned) noexcept override {}
  bool live_idle() const noexcept override { return true; }
  void hold_note(bool, unsigned, unsigned, unsigned) noexcept override {}
  void audition(bool, unsigned) noexcept override {}
  std::uint32_t live_position() const noexcept override { return 0; }
};

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<audio::VoicePool> voices = std::make_unique<audio::VoicePool>();
  std::unique_ptr<model::ModelArena> arena = std::make_unique<model::ModelArena>();
  std::vector<std::int16_t> memory = std::vector<std::int16_t>(60000);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  std::unique_ptr<app::SampleBankMem> mem = std::make_unique<app::SampleBankMem>();
  app::SampleBank bank;
  IdlePreview preview;
  core::ParamRing ring;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);

  Rig() {
    arena->project.reset();
    bank.init(*voices, *arena, *mem, memory.data(), static_cast<std::uint32_t>(memory.size()));
    model::Instrument in;
    model::init_instrument(in, model::InstrumentKind::Sample);
    std::strcpy(in.name, "TONE");
    in.sampler.volume = 80;
    NEED_VOID(arena->project.set_instrument(kSlot, in) == model::Status::Ok);
    std::uint32_t cap = 0;
    std::int16_t* p = bank.reserve(cap);
    NEED_VOID(p != nullptr);
    for (std::uint32_t i = 0; i < kFrames; ++i) {
      p[i] = static_cast<std::int16_t>((i % 64u) * 400 - 12800);
    }
    NEED_VOID(bank.commit_load(kSlot, p, kFrames));
    bank.rebind_now(kSlot);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(arena->project, journal);
    app->attach_document(editor.get());
    app->attach_samples(&bank);
    app->attach_preview(&preview);
    app->attach_param_ring(&ring);
    (void)app->invoke(core::action_id("instrument.select"), static_cast<core::ActionArg>(kSlot + 1));
    pass();
  }
  // CHECK, not NEED, in a constructor: a failed setup shows up in the first check that follows.
  static void NEED_VOID(bool ok) { CHECK_MESSAGE(ok, "rig setup"); }
  // The render's side of a pass: it drains the ParamChange ring into the voices between two main-loop passes (the engine does it every block). Off, the
  // changes stay queued, and the bank's handover waits for them (the review fix: the two writers of a descriptor never overlap).
  bool render_drains = true;
  void drain() {
    core::ParamChange c;
    while (ring.pop(c)) {
      std::uint32_t pan_tracks = 0;
      (void)voices->apply_param(core::param_index(c.id), core::param_field(c.id), c.value, pan_tracks);
    }
  }
  void pass(unsigned ms = 16) {
    clock.advance(ms * core::kTicksPerMs);
    if (render_drains) {
      drain();
    }
    app->run_once();
  }
  void passes(unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
      pass();
    }
  }
  void tap(Button b) {
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true));
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false));
    pass();
  }
  void turn(int detents) {
    input.push(core::make_encoder_event(clock.now(), 0, static_cast<std::int16_t>(detents)));
    pass();
  }
  // The Params screen on `page`, the cursor on `row`.
  void go_to(unsigned page, unsigned row) {
    tap(Button::Params);
    (void)app->invoke(core::action_id("param.group"), static_cast<core::ActionArg>(page));
    for (unsigned i = 0; i < 8; ++i) {
      tap(Button::Up);  // the cursor keeps its row from the page before: to the top
    }
    for (unsigned i = 0; i < row; ++i) {
      tap(Button::Down);
    }
  }
  const audio::SampleDesc& desc() const { return voices->sample(kSlot); }
  const model::SamplerParams& sp() const { return arena->project.instrument(kSlot)->sampler; }
};

}  // namespace

TEST_CASE("live params (app): an edit of a field the descriptor is built from marks the slot, and the engine's descriptor has it once playback is stopped and idle") {
  Rig rig;
  const std::int32_t before = rig.desc().gain;
  CHECK(before == audio::level_gain_q16(80, audio::LevelLaw::Og12));
  rig.go_to(ip::GroupVoice, 0);  // VOLUME
  rig.turn(-5);
  CHECK(rig.sp().volume == 75u);
  CHECK(rig.bank.pending(kSlot));  // marked on the edit
  rig.passes(4);                   // the settle (5 ms of silence, stopped, no live note)
  CHECK_FALSE(rig.bank.pending(kSlot));
  CHECK(rig.desc().gain == audio::level_gain_q16(75, audio::LevelLaw::Og12));
  CHECK(rig.desc().gain != before);
  // Pan, tune and the filter are in the same descriptor.
  rig.turn(0);
  rig.tap(Button::Down);  // PANNING
  rig.turn(10);
  rig.tap(Button::Down);  // TUNE
  rig.turn(3);
  rig.passes(4);
  CHECK(rig.desc().pan == rig.sp().panning);
  CHECK(rig.desc().tune == 3);
  rig.go_to(ip::GroupFilter, 0);  // ENABLE
  rig.turn(1);
  rig.tap(Button::Down);
  rig.tap(Button::Down);  // CUTOFF
  rig.turn(-20);
  rig.passes(4);
  CHECK(rig.desc().filter.enable == (rig.sp().filter_enable != 0u));
  CHECK(rig.desc().filter.cutoff == rig.sp().cutoff);
}

TEST_CASE("live params (app): the bank's handover waits while the ParamChange ring still holds a change, so it never overlaps the render's apply") {
  Rig rig;
  rig.render_drains = false;
  rig.go_to(ip::GroupVoice, 0);  // VOLUME
  rig.turn(-5);
  CHECK(rig.bank.pending(kSlot));
  rig.passes(6);  // stopped and idle, but the ring is not empty: no handover
  CHECK(rig.bank.pending(kSlot));
  CHECK_FALSE(rig.ring.empty());
  rig.drain();  // the render's drain
  rig.passes(4);
  CHECK_FALSE(rig.bank.pending(kSlot));
  CHECK(rig.desc().gain == audio::level_gain_q16(rig.sp().volume, audio::LevelLaw::Og12));
}

TEST_CASE("live params (app): which fields the descriptor reads; an overdrive edit asks for a rebuild (OS-IFX1), a field it does not hold does not") {
  Rig rig;
  rig.go_to(ip::GroupSends, 2);  // OVERDRIVE: in the descriptor since OS-IFX1 (the sends since OS-SENDFX)
  rig.turn(5);
  CHECK(rig.bank.rebind_pending());
  CHECK(app::sample_desc_reads(ip::Volume));
  CHECK(app::sample_desc_reads(ip::Resonance));
  CHECK(app::sample_desc_reads(ip::Glide));  // OS-IFX1: the glide, the overdrive and the bit depth are the descriptor's
  CHECK(app::sample_desc_reads(ip::DelaySend));  // OS-SENDFX: the instrument's sends reach the note's track
  CHECK(app::sample_desc_reads(ip::ReverbSend));
  CHECK(app::sample_desc_reads(ip::BitDepth));
  CHECK(app::sample_desc_reads(ip::Overdrive));
  CHECK_FALSE(app::sample_desc_reads(ip::PlayEngine));
  CHECK(app::sample_desc_reads(ip::envelope(0, ip::EnvRelease)));
  CHECK(app::sample_desc_reads(ip::envelope(1, ip::EnvAttack)));  // OS-MOD: the pan (1) and finetune (5) modulators are in the descriptor too
  CHECK(app::sample_desc_reads(ip::envelope(2, ip::EnvSustain)));
  CHECK(app::sample_desc_reads(ip::envelope(3, ip::EnvEnable)));  // OS-GRAN: the wavetable and granular position slots are in the descriptor
  CHECK(app::sample_desc_reads(ip::envelope(4, ip::EnvAmount)));
  CHECK(app::sample_desc_reads(ip::envelope(5, ip::EnvEnable)));
  CHECK_FALSE(app::sample_desc_reads(ip::kModuleFirst));
  CHECK_FALSE(app::sample_desc_reads(ip::envelope(2, 6)));  // the stride's spare fields
}

// ---- The live update and the rebuild agree -------------------------------------------------------------------------------------------

namespace {

// The first field in which two descriptors differ ("" when they are equal): the parameter fields and the ones the instrument's flags decide.
const char* differs(const audio::SampleDesc& a, const audio::SampleDesc& b) {
#define O3_SAME(f) \
  if (!(a.f == b.f)) return #f
  O3_SAME(data);
  O3_SAME(length);
  O3_SAME(loop_start);
  O3_SAME(loop_end);
  O3_SAME(gain);
  O3_SAME(tune);
  O3_SAME(fine_tune);
  O3_SAME(pan);
  O3_SAME(engine);
  O3_SAME(amiga_vol);
  O3_SAME(env.delay);
  O3_SAME(env.attack);
  O3_SAME(env.hold);
  O3_SAME(env.decay);
  O3_SAME(env.release);
  O3_SAME(env.enable);
  O3_SAME(env.lfo);
  O3_SAME(env.sustain);
  O3_SAME(env.amount);
  O3_SAME(filter.enable);
  O3_SAME(filter.wanted);
  O3_SAME(filter.type);
  O3_SAME(filter.cutoff);
  O3_SAME(filter.resonance);
  O3_SAME(filter.mod.kind);
  O3_SAME(filter.mod.shape);
  O3_SAME(filter.mod.rate);
  O3_SAME(filter.mod.lfo);
  O3_SAME(filter.mod.amount);
  O3_SAME(filter.mod.sustain);
  O3_SAME(filter.mod.lfo_amount);
  O3_SAME(filter.mod.delay);
  O3_SAME(filter.mod.attack);
  O3_SAME(filter.mod.hold);
  O3_SAME(filter.mod.decay);
  O3_SAME(filter.mod.release);
  // OS-MOD: the pan (slot 1), finetune (slot 5) and amp LFO (slot 0) modulators
  O3_SAME(pan_mod.kind);
  O3_SAME(pan_mod.shape);
  O3_SAME(pan_mod.rate);
  O3_SAME(pan_mod.lfo);
  O3_SAME(pan_mod.amount);
  O3_SAME(pan_mod.sustain);
  O3_SAME(pan_mod.lfo_amount);
  O3_SAME(pan_mod.delay);
  O3_SAME(pan_mod.attack);
  O3_SAME(pan_mod.hold);
  O3_SAME(pan_mod.decay);
  O3_SAME(pan_mod.release);
  O3_SAME(tune_mod.kind);
  O3_SAME(tune_mod.shape);
  O3_SAME(tune_mod.rate);
  O3_SAME(tune_mod.lfo);
  O3_SAME(tune_mod.amount);
  O3_SAME(tune_mod.sustain);
  O3_SAME(tune_mod.lfo_amount);
  O3_SAME(tune_mod.delay);
  O3_SAME(tune_mod.attack);
  O3_SAME(tune_mod.hold);
  O3_SAME(tune_mod.decay);
  O3_SAME(tune_mod.release);
  O3_SAME(amp_mod.kind);
  O3_SAME(amp_mod.shape);
  O3_SAME(amp_mod.rate);
  O3_SAME(amp_mod.lfo);
  O3_SAME(amp_mod.amount);
  O3_SAME(amp_mod.lfo_amount);
#undef O3_SAME
  return "";
}

std::uint32_t g_seed = 12345u;
std::uint32_t rnd() {
  g_seed = g_seed * 1664525u + 1013904223u;
  return g_seed >> 8;
}
float frac() { return static_cast<float>(rnd() % 1001u) / 1000.0f; }

// A random instrument inside the model's ranges (the Params tables'): the values the project can hold.
void randomize_params(model::SamplerParams& p) {
  p.volume = static_cast<std::uint8_t>(rnd() % 101u);
  p.panning = static_cast<std::uint8_t>(rnd() % 101u);
  p.tune = static_cast<std::int8_t>(static_cast<int>(rnd() % 49u) - 24);
  p.fine_tune = static_cast<std::int8_t>(static_cast<int>(rnd() % 201u) - 100);
  p.filter_enable = static_cast<std::uint8_t>(rnd() % 2u);
  p.filter_type = static_cast<std::uint8_t>(rnd() % 3u);
  p.cutoff = frac();
  p.resonance = frac() * 4.3f;
  p.delay_send = static_cast<std::uint8_t>(rnd() % 101u);  // OS-SENDFX
  p.reverb_send = static_cast<std::uint8_t>(rnd() % 101u);
  for (const unsigned slot : {0u, 1u, 2u, 5u}) {
    model::Envelope& e = p.envelope[slot];
    e.enable = static_cast<std::uint8_t>(rnd() % 2u);
    e.amount = slot == 0u ? frac() : frac() * 2.0f - 1.0f;
    e.attack = static_cast<std::uint16_t>(rnd() % 6000u);
    e.decay = static_cast<std::uint16_t>(rnd() % 6000u);
    e.sustain = frac();
    e.release = static_cast<std::uint16_t>(rnd() % 6000u);
  }
}

// The Params-table value of a field of the instrument (what write_param stores and send_param_change sends).
float field_value(const model::SamplerParams& p, unsigned field) {
  switch (field) {
    case ip::Volume: return p.volume;
    case ip::Panning: return p.panning;
    case ip::Tune: return p.tune;
    case ip::FineTune: return p.fine_tune;
    case ip::FilterEnable: return p.filter_enable;
    case ip::FilterType: return p.filter_type;
    case ip::Cutoff: return p.cutoff;
    case ip::Resonance: return p.resonance;
    case ip::DelaySend: return p.delay_send;  // OS-SENDFX
    case ip::ReverbSend: return p.reverb_send;
    case ip::Glide: return p.glide;  // OS-IFX1
    case ip::Overdrive: return p.overdrive;
    case ip::BitDepth: return p.bit_depth;
    default: break;
  }
  const unsigned e = (field - ip::kEnvelopeFirst) / ip::kEnvelopeStride;
  const model::Envelope& env = p.envelope[e];
  switch ((field - ip::kEnvelopeFirst) % ip::kEnvelopeStride) {
    case ip::EnvEnable: return env.enable;
    case ip::EnvAmount: return env.amount;
    case ip::EnvAttack: return env.attack;
    case ip::EnvDecay: return env.decay;
    case ip::EnvSustain: return env.sustain;
    default: return env.release;
  }
}

}  // namespace

TEST_CASE("live params: the descriptor the engine updates field by field equals the one rebuilt from the project, over random instruments, flags and both laws") {
  static std::int16_t pcm[4000];
  for (unsigned i = 0; i < 4000; ++i) {
    pcm[i] = static_cast<std::int16_t>(i * 7u);
  }
  std::unique_ptr<audio::VoicePool> live = std::make_unique<audio::VoicePool>();
  std::unique_ptr<audio::VoicePool> built = std::make_unique<audio::VoicePool>();
  unsigned compared = 0;
  for (unsigned round = 0; round < 300; ++round) {
    const audio::LevelLaw law = (round & 1u) != 0u ? audio::LevelLaw::Og1114 : audio::LevelLaw::Og12;
    live->set_law(law);
    model::Instrument target;
    model::init_instrument(target, model::InstrumentKind::Sample);
    randomize_params(target.sampler);
    // What the instrument has apart from the parameters: loop bytes, the delay and hold, the LFO slot 2's fields, all the same before and after.
    for (const unsigned slot : {0u, 1u, 2u, 5u}) {  // (OS-MOD: the pan, finetune and amp slots as well as the cutoff's)
      target.sampler.envelope[slot].loop = static_cast<std::uint8_t>(rnd() % 2u);
      target.sampler.envelope[slot].delay = static_cast<std::uint16_t>(rnd() % 3000u);
      target.sampler.envelope[slot].hold = static_cast<std::uint16_t>(rnd() % 3000u);
      target.sampler.lfo[slot].shape = static_cast<std::uint8_t>(rnd() % 5u);
      target.sampler.lfo[slot].speed = static_cast<std::uint8_t>(rnd() % 29u);
      target.sampler.lfo[slot].amount = frac();
    }
    model::Instrument before = target;  // the same, with other parameter values
    randomize_params(before.sampler);
    for (const unsigned slot : {0u, 1u, 2u, 5u}) {  // the loop bytes and the non-parameter fields are the target's
      const model::Envelope keep = target.sampler.envelope[slot];
      model::Envelope& b = before.sampler.envelope[slot];
      b.loop = keep.loop;
      b.delay = keep.delay;
      b.hold = keep.hold;
    }
    const audio::SampleDesc d0 = app::make_sample_desc(before, pcm, 4000, nullptr, nullptr, law);
    live->set_sample(1, d0);
    NEED(live->sample(1).length != 0u);
    // The Params edits, every field the descriptor reads, in an order of the round's own.
    unsigned fields[40];
    unsigned n = 0;
    for (unsigned f = 0; f < ip::kBase; ++f) {
      if (app::sample_desc_reads(f)) {
        fields[n++] = f;
      }
    }
    for (const unsigned e : {0u, 1u, 2u, 5u}) {
      for (unsigned f = 0; f < ip::kEnvelopeFields; ++f) {
        fields[n++] = ip::envelope(e, f);
      }
    }
    for (unsigned i = n; i > 1; --i) {
      const unsigned j = rnd() % i;
      const unsigned t = fields[i - 1];
      fields[i - 1] = fields[j];
      fields[j] = t;
    }
    for (unsigned i = 0; i < n; ++i) {
      std::uint32_t pan_tracks = 0;
      CHECK(live->apply_param(1, fields[i], field_value(target.sampler, fields[i]), pan_tracks));
    }
    built->set_sample(1, app::make_sample_desc(target, pcm, 4000, nullptr, nullptr, law));
    const char* why = differs(live->sample(1), built->sample(1));
    CHECK_MESSAGE(why[0] == '\0', "round " << round << ": " << why);
    compared += why[0] == '\0' ? 1u : 0u;
  }
  CHECK(compared == 300u);
}

TEST_CASE("live params: every field the model has is either held by the descriptor or ignored, and the ones sample_desc_reads names are the ones the engine applies") {
  audio::SampleDesc d;
  audio::SampleDesc dw;  // OS-GRAN: the position slots belong to a wavetable (slot 3) and to a granular instrument (slot 4): the descriptor of that kind applies them
  dw.wt_size = 256;
  dw.wt_windows = 4;
  audio::SampleDesc dg;
  dg.granular = true;
  for (unsigned id = 0; id < 0x100; ++id) {
    const bool known = id < ip::kBase || (id >= ip::kEnvelopeFirst && (id - ip::kEnvelopeFirst) / ip::kEnvelopeStride < ip::kEnvelopes && (id - ip::kEnvelopeFirst) % ip::kEnvelopeStride < ip::kEnvelopeFields) ||
                       ip::is_mod_field(id);  // (OS-LFOLIVE: the MOD page's SOURCE and LFO rows are live too)
    const unsigned slot = ip::is_mod_field(id) ? (id - ip::kModFirst) / ip::kModStride : (id >= ip::kEnvelopeFirst ? (id - ip::kEnvelopeFirst) / ip::kEnvelopeStride : 0u);
    const audio::InstApply r = audio::apply_instrument_param(slot == 3u ? dw : (slot == 4u ? dg : d), id, 1.0f, audio::LevelLaw::Og12);
    CHECK_MESSAGE((r != audio::InstApply::Unknown) == known, "field " << id);
    CHECK_MESSAGE((r == audio::InstApply::Applied) == app::sample_desc_reads(id), "field " << id);  // (OS-LFOLIVE: the MOD rows are applied by the ring now: all that the bank reads, the engine takes)
    model::ParamField f;
    // the model has exactly these, the nine rows of the source page (OS-INSTUI S3: ids 0x14.., which the engine's table does not take: an edit rebinds the slot instead) and the 24 of the MOD page (S6: 0x60.., the same)
    CHECK_MESSAGE(model::find_param(core::param_id(core::ParamKind::Instrument, 0, id), f) == (known || ip::is_source_field(id) || ip::is_mod_field(id) || ip::is_midi_row(id)), "field " << id);
  }
}

TEST_CASE("live params (app): a Params edit applied live to the engine's descriptor is what the bank's rebuild makes of it when playback stops") {
  Rig rig;
  rig.passes(4);
  rig.render_drains = false;  // the changes stay queued: this case applies them below, as the engine would
  rig.go_to(ip::GroupVoice, 0);
  rig.turn(-7);  // VOLUME
  rig.tap(Button::Down);
  rig.turn(-20);  // PANNING
  rig.go_to(ip::GroupFilter, 0);
  rig.turn(1);  // FILTER ENABLE
  rig.tap(Button::Down);
  rig.turn(1);  // TYPE
  rig.tap(Button::Down);
  rig.turn(-30);  // CUTOFF
  rig.go_to(ip::GroupEnvelope, 0);
  rig.tap(Button::Down);  // (row 0 is SOURCE since OS-INSTUI S6: Envelope already, one click up would make it an LFO)
  rig.turn(-25);  // AMOUNT
  rig.tap(Button::Down);
  rig.turn(40);  // ATTACK
  // What the engine would have been told: the ring's changes, applied as the engine does (no pass of the bank has run since: the rebuild is pending).
  core::ParamChange c;
  unsigned heard = 0;
  while (rig.ring.pop(c)) {
    std::uint32_t pan_tracks = 0;
    CHECK(rig.voices->apply_param(core::param_index(c.id), core::param_field(c.id), c.value, pan_tracks));
    ++heard;
  }
  CHECK(heard >= 6u);
  const audio::SampleDesc live = rig.desc();
  CHECK(rig.bank.rebind_pending());
  rig.render_drains = true;
  rig.passes(5);  // the bank's handover: stopped, idle, the ring drained
  CHECK_FALSE(rig.bank.rebind_pending());
  CHECK(std::string(differs(live, rig.desc())) == "");
  CHECK(rig.desc().filter.enable);
  CHECK(rig.desc().env.enable);
}

TEST_CASE("live params (app): the MOD page's SOURCE and LFO rows are live ParamChanges (OS-LFOLIVE), and applied they are what the bank's rebuild makes of them") {
  Rig rig;
  rig.passes(4);
  rig.render_drains = false;
  rig.go_to(ip::GroupEnvelope, 0);
  const_cast<app::ViewState&>(rig.app->state()).params.env = 2;  // the cutoff's destination
  const_cast<app::ViewState&>(rig.app->state()).params.row = 0;
  rig.pass();
  rig.turn(2);  // SOURCE: LFO
  rig.tap(Button::Down);
  rig.turn(1);  // SHAPE
  rig.tap(Button::Down);
  rig.turn(7);  // SPEED
  rig.tap(Button::Down);
  rig.turn(30);  // AMOUNT
  CHECK(rig.sp().envelope[2].loop == 1u);
  CHECK(rig.sp().lfo[2].speed == 7u);
  bool source = false, shape = false, speed = false, amount = false;
  core::ParamChange c;
  while (rig.ring.pop(c)) {
    const unsigned f = core::param_field(c.id);
    if (f == ip::mod(2, ip::ModSource) && c.value == 2.0f) source = true;
    shape = shape || f == ip::mod(2, ip::LfoShape);
    speed = speed || (f == ip::mod(2, ip::LfoSpeed) && c.value == 7.0f);
    amount = amount || f == ip::mod(2, ip::LfoAmount);
    std::uint32_t pan_tracks = 0;
    CHECK(rig.voices->apply_param(core::param_index(c.id), f, c.value, pan_tracks));
  }
  CHECK(source);
  CHECK(shape);
  CHECK(speed);
  CHECK(amount);
  const audio::SampleDesc live = rig.desc();
  CHECK(live.filter.mod.kind == audio::kFmodLfo);
  CHECK(live.filter.mod.rate == 7);
  CHECK(rig.bank.rebind_pending());  // the rebuild stays the safety net
  rig.render_drains = true;
  rig.passes(5);
  CHECK_FALSE(rig.bank.rebind_pending());
  CHECK(std::string(differs(live, rig.desc())) == "");
}

TEST_CASE("live params: the LFO fields applied live to a descriptor are the rebuild's, over random instruments, every slot, every source and both laws") {
  static std::int16_t pcm[4000];
  for (unsigned i = 0; i < 4000; ++i) {
    pcm[i] = static_cast<std::int16_t>(i * 7u);
  }
  std::unique_ptr<audio::VoicePool> live = std::make_unique<audio::VoicePool>();
  std::unique_ptr<audio::VoicePool> built = std::make_unique<audio::VoicePool>();
  unsigned compared = 0;
  for (unsigned round = 0; round < 300; ++round) {
    const unsigned kind = round % 3u;  // 0 sample, 1 wavetable, 2 granular: slots 3 and 4 belong to the last two
    model::Instrument target;
    model::init_instrument(target, model::InstrumentKind::Sample);
    randomize_params(target.sampler);
    target.sampler.play_mode = kind == 1u ? 6u : (kind == 2u ? 7u : 0u);
    target.sampler.wavetable_window_size = 256;
    target.sampler.wavetable_window = static_cast<std::uint16_t>(rnd() % 10u);
    unsigned slots[5] = {0, 1, 2, 5, kind == 1u ? 3u : 4u};
    for (const unsigned e : slots) {
      model::Envelope& env = target.sampler.envelope[e];
      const unsigned src = rnd() % 3u;
      env.enable = src != 0u ? 1u : 0u;
      env.loop = src == 2u ? 1u : 0u;
      env.delay = static_cast<std::uint16_t>(rnd() % 3000u);
      env.hold = static_cast<std::uint16_t>(rnd() % 3000u);
      target.sampler.lfo[e].shape = static_cast<std::uint8_t>(rnd() % 5u);
      target.sampler.lfo[e].speed = static_cast<std::uint8_t>(rnd() % (e == 0u ? 24u : 29u));
      target.sampler.lfo[e].amount = frac();
    }
    model::Instrument before = target;  // the same instrument with other SOURCE and LFO values (and the other fields the same: only these are applied)
    for (const unsigned e : slots) {
      const unsigned src = rnd() % 3u;
      before.sampler.envelope[e].enable = src != 0u ? 1u : 0u;
      before.sampler.envelope[e].loop = src == 2u ? 1u : 0u;
      before.sampler.lfo[e].shape = static_cast<std::uint8_t>(rnd() % 5u);
      before.sampler.lfo[e].speed = static_cast<std::uint8_t>(rnd() % (e == 0u ? 24u : 29u));
      before.sampler.lfo[e].amount = frac();
      before.sampler.envelope[e].amount = target.sampler.envelope[e].amount;  // (the ADSR's amount is the sender's EnvAmount after a switch to Envelope: below)
    }
    live->set_sample(1, app::make_sample_desc(before, pcm, 4000, nullptr, nullptr, audio::LevelLaw::Og12));
    NEED(live->sample(1).length != 0u);
    for (const unsigned e : slots) {
      std::uint32_t pan_tracks = 0;
      const model::Envelope& env = target.sampler.envelope[e];
      const unsigned src = env.enable == 0u ? 0u : (env.loop != 0u ? 2u : 1u);
      // what app::send_param_change sends: the SOURCE first, then what the new generator reads, then the other fields in any order
      CHECK(live->apply_param(1, ip::mod(e, ip::ModSource), static_cast<float>(src), pan_tracks));
      if (src != 2u && e != 0u) {
        CHECK(live->apply_param(1, ip::envelope(e, ip::EnvAmount), env.amount, pan_tracks));
      }
      const unsigned order = rnd() % 6u;
      const unsigned fs[3][6] = {{0, 0, 1, 1, 2, 2}, {1, 2, 0, 2, 0, 1}, {2, 1, 2, 0, 1, 0}};
      for (unsigned k = 0; k < 3u; ++k) {
        const unsigned f = fs[k][order];
        const float v = f == 0u ? static_cast<float>(target.sampler.lfo[e].shape) : (f == 1u ? static_cast<float>(target.sampler.lfo[e].speed) : target.sampler.lfo[e].amount);
        CHECK(live->apply_param(1, ip::mod(e, ip::LfoShape + f), v, pan_tracks));
      }
    }
    built->set_sample(1, app::make_sample_desc(target, pcm, 4000, nullptr, nullptr, audio::LevelLaw::Og12));
    const audio::SampleDesc& a = live->sample(1);
    const audio::SampleDesc& b = built->sample(1);
    const char* why = differs(a, b);
    CHECK_MESSAGE(why[0] == '\0', "round " << round << ": " << why);
    // the position slot's modulator (not in differs): the same
    const bool same_pos = a.pos_mod.kind == b.pos_mod.kind && a.pos_mod.shape == b.pos_mod.shape && a.pos_mod.rate == b.pos_mod.rate && a.pos_mod.lfo == b.pos_mod.lfo &&
                          a.pos_mod.amount == b.pos_mod.amount && a.pos_mod.lfo_amount == b.pos_mod.lfo_amount;
    CHECK_MESSAGE(same_pos, "round " << round << ": pos_mod");
    compared += why[0] == '\0' ? 1u : 0u;
  }
  CHECK(compared == 300u);
}

// OS-CEWTENV W3a (app): make_sample_desc maps a WT loop (exactly 2 or 3) on the cutoff slot to the WT kinds, with the contour, the warps and the sustain, and makes the
// contour into the bound bank before the descriptor is staged; loop 0, 1 or a garbage byte never maps to a WT kind. W3b: the pan, the finetune and the position slots map the same way; the amp slot follows in W4.
TEST_CASE("OS-CEWTENV W3a (app): make_sample_desc maps a WT loop on the cutoff to the WT kinds and makes the contour; loop 0, 1 or a garbage byte does not") {
  static std::int16_t pcm[4000];
  for (std::uint32_t i = 0; i < 4000; ++i) {
    pcm[i] = static_cast<std::int16_t>((i % 128u) * 200 - 12800);
  }
  static audio::WtBank bank;  // .bss: 64 KiB never on a stack
  audio::wt_bind(bank);
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Sample);
  in.sampler.filter_enable = 1;
  in.sampler.filter_type = 0;  // low-pass
  in.sampler.cutoff = 0.5f;
  in.sampler.resonance = 0.0f;
  // slot 2 (the cutoff): loop 2 (WT Env), contour 37, bend 0x9C, sust 40, decay 500 ms, release 100 ms
  in.sampler.envelope[2].loop = 2;
  in.sampler.envelope[2].enable = 1;
  in.sampler.envelope[2].decay = 500;
  in.sampler.envelope[2].release = 100;
  in.sampler.lfo[2].shape = 37;
  in.sampler.lfo[2].speed = 9;
  in.sampler.lfo[2].amount = 0.5f;
  in.sampler.lfo[2].wt_bend = 0x9C;
  in.sampler.lfo[2].wt_sust = 40;
  // slot 1 (the pan): loop 3, a contour of 200 — W3b: the pan slot maps to the WT LFO with the contour, the warps and the sustain
  in.sampler.envelope[1].loop = 3;
  in.sampler.envelope[1].enable = 1;
  in.sampler.lfo[1].shape = 200;
  in.sampler.lfo[1].speed = 5;
  in.sampler.lfo[1].amount = 0.3f;
  in.sampler.lfo[1].wt_bend = 0x17;
  in.sampler.lfo[1].wt_sust = 99;
  // slot 5 (the finetune): loop 0 with junk WT bytes
  in.sampler.envelope[5].loop = 0;
  in.sampler.envelope[5].enable = 1;
  in.sampler.lfo[5].wt_bend = 0xFF;
  in.sampler.lfo[5].wt_sust = 200;
  // slot 0 (the amp): a garbage loop byte 200 is not a WT slot
  in.sampler.envelope[0].loop = 200;
  in.sampler.envelope[0].enable = 1;
  in.sampler.lfo[0].shape = 3;
  in.sampler.lfo[0].speed = 4;
  in.sampler.lfo[0].amount = 0.25f;

  const audio::SampleDesc d = app::make_sample_desc(in, pcm, 4000, nullptr, nullptr, audio::LevelLaw::Og12);
  static o3os::test::StaticSlot<audio::VoicePool> vslot;  // set_sample sanitizes: the checks on the other slots read the pool's copy
  audio::VoicePool& vp = vslot.make();
  vp.set_sample(0, d);
  const audio::SampleDesc& sd = vp.sample(0);
  CHECK(d.filter.mod.kind == audio::kFmodWtEnv);
  CHECK(d.filter.mod.shape == 37);
  CHECK(d.filter.mod.wt_bend == 0x9C);
  CHECK(d.filter.mod.wt_sust == 40);
  CHECK(d.filter.mod.rate == 9);
  CHECK(d.filter.mod.decay == 500);
  CHECK(d.filter.mod.release == 100);
  CHECK(d.filter.mod.amount == 0.5f);
  CHECK(audio::wt_eval_shape(37, 0.0f, 0.0f) == audio::wt_eval(audio::wt_contour(bank, 37), 0.0f, 0.0f));  // the contour was made into the bound bank
  CHECK(sd.pan_mod.kind == audio::kFmodWtLfo);  // loop 3 on the pan (W3b): the WT LFO, the contour verbatim, the warps and the sustain kept by the sanitizing
  CHECK(sd.pan_mod.shape == 200);
  CHECK(sd.pan_mod.wt_bend == 0x17u);
  CHECK(sd.pan_mod.wt_sust == 99u);
  CHECK(sd.tune_mod.kind == audio::kFmodEnv);
  CHECK(sd.tune_mod.wt_bend == 0u);
  CHECK(sd.tune_mod.wt_sust == 0u);
  CHECK(sd.amp_mod.kind == audio::kFmodLfo);  // loop 200: loop != 0, the amp slot is a tremolo as before
  CHECK(sd.filter.mod.kind == audio::kFmodWtEnv);  // and the cutoff's WT kind, contour, warps and sustain survive the sanitizing
  CHECK(sd.filter.mod.shape == 37);
  CHECK(sd.filter.mod.wt_bend == 0x9C);
  CHECK(sd.filter.mod.wt_sust == 40);
  // loop 3 on the cutoff: the WT LFO kind
  in.sampler.envelope[2].loop = 3;
  const audio::SampleDesc d3 = app::make_sample_desc(in, pcm, 4000, nullptr, nullptr, audio::LevelLaw::Og12);
  CHECK(d3.filter.mod.kind == audio::kFmodWtLfo);
  CHECK(d3.filter.mod.shape == 37);
  CHECK(d3.filter.mod.wt_bend == 0x9C);
  CHECK(d3.filter.mod.wt_sust == 40);
  // loop 0 on the cutoff with junk WT bytes: an ADSR, the bytes dropped
  in.sampler.envelope[2].loop = 0;
  in.sampler.lfo[2].wt_bend = 0xFF;
  in.sampler.lfo[2].wt_sust = 200;
  const audio::SampleDesc d0 = app::make_sample_desc(in, pcm, 4000, nullptr, nullptr, audio::LevelLaw::Og12);
  CHECK(d0.filter.mod.kind == audio::kFmodEnv);
  CHECK(d0.filter.mod.wt_bend == 0u);
  CHECK(d0.filter.mod.wt_sust == 0u);
  // a garbage loop byte on the cutoff: not a WT slot, an LFO as before
  in.sampler.envelope[2].loop = 200;
  const audio::SampleDesc d200 = app::make_sample_desc(in, pcm, 4000, nullptr, nullptr, audio::LevelLaw::Og12);
  CHECK(d200.filter.mod.kind == audio::kFmodLfo);
  CHECK(d200.filter.mod.wt_bend == 0u);
  CHECK(d200.filter.mod.wt_sust == 0u);

  // OS-CEWTENV W4: the amp slot. Loop 2 (WT Env): the kind, the contour (the slot's lfo[0] shape), the warps, the sustain, the duration and the release in ms; no ADSR.
  in.sampler.envelope[0].loop = 2;
  in.sampler.envelope[0].enable = 1;
  in.sampler.envelope[0].decay = 300;
  in.sampler.envelope[0].release = 120;
  in.sampler.lfo[0].shape = 61;
  in.sampler.lfo[0].speed = 7;
  in.sampler.lfo[0].amount = 0.8f;
  in.sampler.lfo[0].wt_bend = 0x23;
  in.sampler.lfo[0].wt_sust = 0;
  const audio::SampleDesc a2 = app::make_sample_desc(in, pcm, 4000, nullptr, nullptr, audio::LevelLaw::Og12);
  CHECK(a2.amp_mod.kind == audio::kFmodWtEnv);
  CHECK(a2.amp_mod.shape == 61);
  CHECK(a2.amp_mod.wt_bend == 0x23);
  CHECK(a2.amp_mod.wt_sust == 0);
  CHECK(a2.amp_mod.decay == 300);
  CHECK(a2.amp_mod.release == 120);
  CHECK_FALSE(a2.env.enable);
  CHECK(audio::wt_eval_shape(61, 0.5f, 0.0f) == audio::wt_eval(audio::wt_contour(bank, 61), 0.5f, 0.0f));  // the contour was made into the bound bank
  vp.set_sample(0, a2);
  CHECK(vp.sample(0).amp_mod.kind == audio::kFmodWtEnv);  // the pool keeps the amp slot's WT kind, the contour, the warps and the times
  CHECK(vp.sample(0).amp_mod.shape == 61);
  CHECK(vp.sample(0).amp_mod.wt_bend == 0x23);
  CHECK(vp.sample(0).amp_mod.decay == 300);
  // a live edit of the amp slot's decay or release moves a WT slot's duration and release (the rebuild equals the live edit); an ADSR slot's FilterMod times stay as they were
  audio::SampleDesc live = a2;
  (void)audio::apply_instrument_param(live, audio::inst_param::envelope(0, audio::inst_param::EnvDecay), 450.0f, audio::LevelLaw::Og12);
  (void)audio::apply_instrument_param(live, audio::inst_param::envelope(0, audio::inst_param::EnvRelease), 90.0f, audio::LevelLaw::Og12);
  CHECK(live.amp_mod.decay == 450);
  CHECK(live.amp_mod.release == 90);
  // loop 3 (WT LFO): the kind, the rate, the depth (the slot's LFO amount)
  in.sampler.envelope[0].loop = 3;
  const audio::SampleDesc a3 = app::make_sample_desc(in, pcm, 4000, nullptr, nullptr, audio::LevelLaw::Og12);
  CHECK(a3.amp_mod.kind == audio::kFmodWtLfo);
  CHECK(a3.amp_mod.rate == 7);
  CHECK(a3.amp_mod.amount == 0.8f);
  CHECK(a3.amp_mod.shape == 61);
  CHECK_FALSE(a3.env.enable);
  // the slot off with a WT loop byte: nothing runs
  in.sampler.envelope[0].enable = 0;
  const audio::SampleDesc a_off = app::make_sample_desc(in, pcm, 4000, nullptr, nullptr, audio::LevelLaw::Og12);
  CHECK(a_off.amp_mod.kind == audio::kFmodNone);
  // loop 0 with junk WT bytes: the amp ADSR, the bytes dropped; an ADSR instrument's descriptor is what it was
  in.sampler.envelope[0].enable = 1;
  in.sampler.envelope[0].loop = 0;
  in.sampler.lfo[0].wt_bend = 0xFF;
  in.sampler.lfo[0].wt_sust = 200;
  const audio::SampleDesc a0 = app::make_sample_desc(in, pcm, 4000, nullptr, nullptr, audio::LevelLaw::Og12);
  CHECK(a0.env.enable);
  CHECK(a0.amp_mod.kind == audio::kFmodNone);
  CHECK(a0.amp_mod.wt_bend == 0u);
  CHECK(a0.amp_mod.wt_sust == 0u);
  CHECK(a0.amp_mod.decay == 0u);
}

// OS-CEWTENV W3b (app): slot 3's enable byte is a bit mask (spec F2, A8): 0x01 is the wavetable position, 0x02 the granular length (W5), so 0x02 alone is no position modulation (F16's second case); the
// rebuild and the live edit (apply_instrument_param) agree on every byte. A non-wavetable instrument never has slot 3's modulator; the WT kinds map on the finetune and on the granular position too.
TEST_CASE("OS-CEWTENV W3b (app): slot 3 honours bit 0x01 for the wavetable position; a non-wavetable instrument never runs it; the finetune and the granular position map the WT kinds") {
  static std::int16_t pcm[4000];
  for (std::uint32_t i = 0; i < 4000; ++i) {
    pcm[i] = static_cast<std::int16_t>((i % 128u) * 200 - 12800);
  }
  static audio::WtBank bank;  // .bss: 64 KiB never on a stack
  audio::wt_bind(bank);
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Sample);
  in.sampler.play_mode = 6;  // wavetable
  in.sampler.wavetable_window_size = 256;
  in.sampler.wavetable_window = 2;
  const auto slot = [&](unsigned e, std::uint8_t enable, std::uint8_t loop) {
    in.sampler.envelope[e].enable = enable;
    in.sampler.envelope[e].loop = loop;
    in.sampler.envelope[e].decay = 500;
    in.sampler.envelope[e].release = 100;
    in.sampler.lfo[e].shape = 37;
    in.sampler.lfo[e].speed = 9;
    in.sampler.lfo[e].amount = 0.5f;
    in.sampler.lfo[e].wt_bend = 0x34;
    in.sampler.lfo[e].wt_sust = 40;
  };
  const auto build = [&] { return app::make_sample_desc(in, pcm, 4000, nullptr, nullptr, audio::LevelLaw::Og12); };
  slot(3, 0x02, 0);
  CHECK(build().wt_windows > 1u);
  CHECK(build().pos_mod.kind == audio::kFmodNone);  // 0x02 alone: the granular length's bit, no position modulation
  slot(3, 0x02, 2);
  CHECK(build().pos_mod.kind == audio::kFmodNone);
  slot(3, 0x01, 2);
  const audio::SampleDesc w1 = build();
  CHECK(w1.pos_mod.kind == audio::kFmodWtEnv);
  CHECK(w1.pos_mod.shape == 37);
  CHECK(w1.pos_mod.wt_bend == 0x34);
  CHECK(w1.pos_mod.wt_sust == 40);
  CHECK(w1.pos_mod.decay == 500);
  slot(3, 0x03, 3);
  CHECK(build().pos_mod.kind == audio::kFmodWtLfo);  // both bits: the position is on
  slot(3, 0x01, 0);
  CHECK(build().pos_mod.kind == audio::kFmodEnv);    // the ADSR as before
  slot(3, 0x00, 2);
  CHECK(build().pos_mod.kind == audio::kFmodNone);
  // the live edit of the enable byte and the rebuild agree on every byte of slot 3
  for (unsigned raw = 0; raw < 8u; ++raw) {
    slot(3, 0x01, 2);
    audio::SampleDesc live = build();
    slot(3, static_cast<std::uint8_t>(raw), 2);
    const audio::SampleDesc built = build();
    (void)audio::apply_instrument_param(live, audio::inst_param::envelope(3, audio::inst_param::EnvEnable), static_cast<float>(raw), audio::LevelLaw::Og12);
    // (on or off: the live edit keeps the slot's ADSR / LFO kind, the WT kind is the rebuild's, which the platform runs at the next quiet moment, spec A6)
    CHECK_MESSAGE((live.pos_mod.kind != audio::kFmodNone) == (built.pos_mod.kind != audio::kFmodNone), "enable byte " << raw);
    CHECK_MESSAGE((live.pos_mod.kind != audio::kFmodNone) == ((raw & 1u) != 0u), "enable byte " << raw);
  }
  // a non-wavetable instrument never runs slot 3 (a sample, a one window wavetable is not made here)
  in.sampler.play_mode = 0;
  slot(3, 0x01, 2);
  CHECK(build().pos_mod.kind == audio::kFmodNone);
  // the granular position (slot 4) maps the WT kinds as well; slot 3 is not its slot
  in.sampler.play_mode = 7;
  slot(3, 0x01, 2);
  slot(4, 0x00, 0);
  CHECK(build().pos_mod.kind == audio::kFmodNone);
  slot(4, 0x01, 3);
  const audio::SampleDesc g = build();
  CHECK(g.pos_mod.kind == audio::kFmodWtLfo);
  CHECK(g.pos_mod.shape == 37);
  CHECK(g.pos_mod.wt_bend == 0x34);
  // the finetune (slot 5) maps them too, and so does the amp slot (0, W4)
  in.sampler.play_mode = 0;
  slot(5, 0x01, 2);
  slot(0, 0x01, 3);
  const audio::SampleDesc f = build();
  CHECK(f.tune_mod.kind == audio::kFmodWtEnv);
  CHECK(f.tune_mod.wt_sust == 40);
  CHECK(f.amp_mod.kind == audio::kFmodWtLfo);  // loop 3 on the amp: the WT LFO (W4)
}

// OS-CEWTENV W3c (app): make_sample_desc bakes a WT slot's warps (audio::wt_bake) and keeps the id in the slot's `delay`; a slot with no warp, or a pool that is full, keeps 0 (the
// render applies the warps live). The same warp of two instruments shares one baked contour.
TEST_CASE("OS-CEWTENV W3c (app): make_sample_desc bakes the warps of a WT slot into the pool and keeps the id; no warp, no id") {
  static std::int16_t pcm[4000];
  for (std::uint32_t i = 0; i < 4000; ++i) {
    pcm[i] = static_cast<std::int16_t>((i % 128u) * 200 - 12800);
  }
  static audio::WtBank bank;
  static audio::WtBakeBank pool;
  audio::wt_bind(bank);
  audio::wt_bake_bind(pool);
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Sample);
  const auto slot = [&](unsigned e, std::uint8_t loop, std::uint8_t shape, std::uint8_t bend) {
    in.sampler.envelope[e].enable = 1;
    in.sampler.envelope[e].loop = loop;
    in.sampler.lfo[e].shape = shape;
    in.sampler.lfo[e].amount = 0.5f;
    in.sampler.lfo[e].wt_bend = bend;
  };
  slot(2, 2, 37, 0x9C);  // the cutoff: a warp
  slot(1, 3, 37, 0x9C);  // the pan: the same shape and warp
  slot(5, 2, 40, 0x00);  // the finetune: no warp
  const audio::SampleDesc d = app::make_sample_desc(in, pcm, 4000, nullptr, nullptr, audio::LevelLaw::Og12);
  CHECK(d.filter.mod.kind == audio::kFmodWtEnv);
  CHECK(d.filter.mod.delay == 1u);
  CHECK(d.pan_mod.delay == 1u);  // one baked contour for the same shape and warp
  CHECK(d.tune_mod.kind == audio::kFmodWtEnv);
  CHECK(d.tune_mod.delay == 0u);  // no warp: the bank's contour as it is
  CHECK(audio::wt_bake_used() == 1u);
  // the baked contour is the live evaluation, resampled
  CHECK(std::fabs(audio::wt_eval_slot(37, d.filter.mod.delay, 0x9C, 0.5f) - audio::wt_eval_shape(37, 0.5f, 0x9C)) < 0.05f);
  // an ADSR slot (loop 0) keeps its own delay and no id
  in.sampler.envelope[2].loop = 0;
  in.sampler.envelope[2].delay = 250;
  const audio::SampleDesc a = app::make_sample_desc(in, pcm, 4000, nullptr, nullptr, audio::LevelLaw::Og12);
  CHECK(a.filter.mod.kind == audio::kFmodEnv);
  CHECK(a.filter.mod.delay == 250u);
}
