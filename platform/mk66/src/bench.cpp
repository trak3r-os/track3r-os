// SPDX-License-Identifier: GPL-3.0-or-later
//
// `diag bench <kernel> [blocks]` (OS-PERF3): the DWT cycle count of one block of a kernel, measured on the unit, main loop only. The kernels are the ones the render spends its block
// on (the sampler voice, a TB-303 instance, the two send effects, the master limiter, the strips) plus a memory characterisation. THE POINT IS A NUMBER THAT MOVES WHEN A CHANGE
// WORKS: each kernel runs with the same input and the same settings every time, `blocks` times, and the minimum is reported (bench_report.h has the line).
//
// HOW A BLOCK IS MEASURED WITHOUT STOPPING THE SOUND. The render is a software interrupt (priority 128); BASEPRI = 128 holds it off for the length of ONE kernel call (a block of the heaviest
// kernel is about 0.5 ms), the call is timed with the cycle counter, BASEPRI goes back, and the loop waits for the next render to finish before it does another. So the audio keeps
// flowing (a block or two late at worst, never two in a row), the measured call is never preempted by the render, and the higher interrupts (the audio DMA, SysTick, USB) that do get in
// show up as a longer max, not in the minimum. Nothing of this runs in an interrupt. The render is never touched while it runs: when the main loop executes, the render is not in the middle
// of anything (it is above the main loop), so calling its objects with the render held off is the same as calling them from the render.
//
// VARIANTS (`bench <kernel> [blocks] alias|direct|sram`, the A/B of the placement work, bench_report.h Variant): the same kernel with its data read through the cached ICode alias of the SDRAM,
// through the uncached 0x7000_0000 address, or (the TB-303) with a copy of its state in SRAM_U. The result line then names the kernel `delay+alias`.
// WHAT IT TOUCHES. The send effects and the limiter run on PRIVATE instances built on the stack for the run (the live ones are the project's), with a ring/memory taken from the unused
// `ui_cache` SDRAM region (512 KiB, nothing else uses it); the same code, the same SRAM_U/SDRAM placement of the state as the live objects. The sampler voice runs on the LIVE pool
// (there is no second copy): it takes TRACK 16 for the run and cuts it at the end, and REFUSES while something is playing (a voice sounding or fading in the pool): stop the transport
// first. The TB-303 runs on a private instance, init()ed afresh for each run (a fixed note from a fixed state: the peak is the same run after run; the live instance is not touched). With the
// transport stopped nothing of the project's sound or state changes. `strips` and `stages` only read the render's own stage figures, and measure whatever is playing.
#include "internal.h"
#include "regs.h"

#include <o3os/audio/bench_stereo.h>
#include <o3os/audio/graph.h>
#include <o3os/audio/kernels.h>
#include <o3os/audio/loop_mix.h>
#include <o3os/audio/loop_overdub.h>
#include <o3os/audio/master_dj_filter.h>
#include <o3os/audio/master_kill_eq.h>
#include <o3os/audio/master_limiter.h>
#include <o3os/audio/module_voices.h>
#include <o3os/audio/send_delay.h>
#include <o3os/audio/send_reverb.h>
#include <o3os/audio/simd.h>
#include <o3os/audio/svf.h>
#include <o3os/audio/voices.h>
#include <o3os/hal/irq.h>
#include <o3os/platform/mk66/audio.h>
#include <o3os/platform/mk66/bench_disk.h>
#include <o3os/platform/mk66/cache_check.h>
#include <o3os/platform/mk66/mem_map.h>
#include <o3os/platform/mk66/sdram_view.h>
#include <o3os/platform/mk66/sdramc_config.h>
#include <o3os/platform/mk66/platform.h>
#include <o3os/platform/mk66/save/bench_scratch.h>
#include <o3os/platform/mk66/sections.h>
#include <o3os/platform/mk66/storage/device.h>
#include <o3os/platform/mk66/usb/usb0.h>

#include <cstring>
#include <new>

namespace o3os::platform::mk66::bench {
namespace {

constexpr std::uint32_t kBlock = hal::audio::kBlock;
constexpr unsigned kWarmup = 12;  // untimed blocks first: the caches, the filters, and a changed reverb quality clearing its tail (13 blocks of silence at most are not measured: see run_reverb)

// ---- the machinery -----------------------------------------------------------------------------------------------------------------
inline std::uint32_t dwt() noexcept { return regs::dwt_cyccnt(); }
inline std::uint32_t basepri() noexcept {
  std::uint32_t v;
  asm volatile("mrs %0, basepri" : "=r"(v));
  return v;
}
inline void set_basepri(std::uint32_t v) noexcept { asm volatile("msr basepri, %0\n\tisb" : : "r"(v) : "memory"); }

// Holds the render off for the life of the object (RAII), and puts back what was there.
class HoldRender {
 public:
  HoldRender() noexcept : saved_(basepri()) { set_basepri(hal::irq::prio::kRender); }
  ~HoldRender() { set_basepri(saved_); }
  HoldRender(const HoldRender&) = delete;
  HoldRender& operator=(const HoldRender&) = delete;

 private:
  std::uint32_t saved_;
};

// Waits (low power, bounded) until a render has finished since `renders`. Without a running audio path there is nothing to wait for.
void wait_render(std::uint32_t& renders) noexcept {
  if (!audio_running()) {
    return;
  }
  const std::uint32_t t0 = dwt();
  audio_main_pass();  // OS-MAINGUARD: the main thread is running here, not starved; the bench's own waits beat, so the guard never sheds a block the bench is measuring
  while (audio_status().snapshot().renders == renders && static_cast<std::uint32_t>(dwt() - t0) < 4u * hal::audio::kCyclesPerBlock) {
    audio_main_pass();
    asm volatile("wfi");
  }
  renders = audio_status().snapshot().renders;
}

// One block of the kernel at a time: `prep` (untimed) and `post` (untimed) around `body` (timed), all with the render held off.
template <class Prep, class Body, class Post>
Stat measure(std::uint32_t blocks, Prep&& prep, Body&& body, Post&& post) noexcept {
  Stat st;
  std::uint32_t renders = audio_status().snapshot().renders;
  for (std::uint32_t i = 0; i < blocks + kWarmup; ++i) {
    wait_render(renders);
    HoldRender hold;
    prep();
    const std::uint32_t t0 = dwt();
    body();
    const std::uint32_t dt = dwt() - t0;
    post();
    if (i >= kWarmup) {
      st.add(dt);
    }
  }
  return st;
}

// A fixed pseudo-random sequence (a 32-bit LCG, Numerical Recipes constants): the same input every run.
struct Noise {
  std::uint32_t s = 12345u;
  std::int32_t next() noexcept {
    s = s * 1664525u + 1013904223u;
    return static_cast<std::int32_t>(s) >> 16;  // -32768..32767
  }
};

struct Out {
  Emit emit;
  void* ctx;
  Variant variant;
  void line(const char* text, std::size_t n) const { emit(ctx, text, n); }
  // The kernel's name with the variant after a '+'.
  const char* tagged(const char* name, char* buf, std::size_t cap) const {
    audio::diag::Text t(buf, cap);
    t.str(name);
    if (variant != Variant::Normal) {
      t.chr('+').str(kVariantName[static_cast<unsigned>(variant)]);
    }
    return buf;
  }
  void stat(const char* name, const Stat& s, const char* note = nullptr) const {
    char tag[40];
    char buf[120];
    line(buf, format_line(tagged(name, tag, sizeof tag), s, note, buf, sizeof buf));
  }
  void na(const char* name, const char* why) const {
    char tag[40];
    char buf[140];
    line(buf, format_na(tagged(name, tag, sizeof tag), why, buf, sizeof buf));
  }
  // The result line of a kernel with an output check: `label` and the peak in its note, then the FAIL line when nothing came out (bench_report.h Peak).
  void stat_checked(const char* name, const Stat& s, const char* label, const Peak& peak, const char* fail_hint = nullptr) const {
    char note[64];
    (void)format_peak_note(label, peak, note, sizeof note);
    stat(name, s, note);
    if (peak.silent()) {
      char tag[40];
      char buf[160];
      line(buf, format_fail(tagged(name, tag, sizeof tag), peak, fail_hint, buf, sizeof buf));
    }
  }
  // Whether this run reaches its SDRAM data through the cached alias: forced by the variant, else as production does (sdram_view.h).
  bool alias() const { return variant == Variant::Alias || (variant == Variant::Normal && mem::kCoreUsesAlias); }
  bool forced() const { return variant == Variant::Alias || variant == Variant::Direct; }
};

// Whether the controller answers at the ICode alias (boot.cpp wrote the mask).
bool alias_enabled() noexcept { return (regs::at<std::uint32_t>(sdram::kCm0Address) & sdram::kAliasBam) == sdram::kAliasBam; }

// The SDRAM the effects' private memory comes from: the unused `ui_cache` region, at its system-bus address or (variant alias) through the cached ICode alias.
constexpr std::uint32_t kBenchDelayFrames = 65536u;  // 256 KB of the region
constexpr std::uint32_t kBenchReverbOffset = 256u * mem::kKiB;
constexpr std::uint32_t kBenchStateOffset = 400u * mem::kKiB;  // a copy of an instance's state
std::uintptr_t scratch_base(const Out& out) noexcept { return out.alias() ? mem::sdram_alias(mem::kUiCacheBase) : mem::kUiCacheBase; }
std::uint32_t* scratch_ring(const Out& out) noexcept { return reinterpret_cast<std::uint32_t*>(scratch_base(out)); }
std::int16_t* scratch_reverb(const Out& out) noexcept { return reinterpret_cast<std::int16_t*>(scratch_base(out) + kBenchReverbOffset); }
static_assert(kBenchReverbOffset + audio::SendReverb::kMemBytes <= kBenchStateOffset && kBenchStateOffset + 1024u <= mem::kUiCacheSize, "the bench's delay ring, reverb memory and state copy fit the ui_cache region");
static_assert(kBenchDelayFrames * 4u == kBenchReverbOffset, "the delay ring is the first 256 KB");

// Room on the main stack for a fixture and its blocks, with a margin: the stack has no guard (README), so the bench checks before it builds one.
bool stack_room(std::uint32_t need) noexcept {
  const std::uint32_t sp = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0)));
  const std::uint32_t floor = mem::kStackTop - mem::kStackSize;
  return sp > floor && sp - floor > need + 2048u;
}

// ---- the kernels -------------------------------------------------------------------------------------------------------------------
void run_limiter(const Out& out, std::uint32_t blocks, bool loud) noexcept {
  const char* name = loud ? "limiter" : "limiter_q";
  if (out.variant != Variant::Normal) {
    out.na(name, "its state is SRAM_U already: no variant");
    return;
  }
  if (!stack_room(sizeof(audio::MasterLimiter) + 1024u)) {
    out.na(name, "not enough stack");
    return;
  }
  alignas(audio::MasterLimiter) unsigned char raw[sizeof(audio::MasterLimiter)];
  audio::MasterLimiter& lim = *new (raw) audio::MasterLimiter();
  lim.reset();
  (void)lim.set_param(audio::limiter::Switch, 1.0f);  // the slow stage on: the worst case of the two
  alignas(8) float l[kBlock];
  alignas(8) float r[kBlock];
  Noise n;
  const float scale = loud ? 1.6f / 32768.0f : 0.2f / 32768.0f;  // loud: peaks above the ceiling everywhere (the fast stage divides every sample); quiet: the identity path
  const Stat st = measure(
      blocks,
      [&] {
        for (std::uint32_t k = 0; k < kBlock; ++k) {
          l[k] = static_cast<float>(n.next()) * scale;
          r[k] = static_cast<float>(n.next()) * scale;
        }
      },
      [&] { lim.process(l, r, kBlock); }, [] {});
  out.stat(name, st);
}

// OS-DJ: the master DJ stages as private instances on the stack, each with its ramps run down (a kill held, the filter at its detent), a block of loud noise through them: what a held kill and a held
// filter cost a block on the unit.  At rest the graph calls none of them (their slots are bypassed), so the rest cost of the page is not in this kernel: `bench stages` in situ shows it.
void run_dj(const Out& out, std::uint32_t blocks) noexcept {
  if (out.variant != Variant::Normal) {
    out.na("dj", "its state is SRAM_U already: no variant");
    return;
  }
  if (!stack_room(sizeof(audio::MasterKillEq) + sizeof(audio::MasterDjFilter) + 1024u)) {
    out.na("dj", "not enough stack");
    return;
  }
  alignas(audio::MasterKillEq) unsigned char raw_kill[sizeof(audio::MasterKillEq)];
  alignas(audio::MasterDjFilter) unsigned char raw_filter[sizeof(audio::MasterDjFilter)];
  alignas(8) float l[kBlock];
  alignas(8) float r[kBlock];
  Noise n;
  const auto fill = [&] {
    for (std::uint32_t k = 0; k < kBlock; ++k) {
      l[k] = static_cast<float>(n.next()) * (0.5f / 32768.0f);
      r[k] = static_cast<float>(n.next()) * (0.5f / 32768.0f);
    }
  };
  const auto nop = [] {};
  struct Arm {
    const char* name;
    unsigned kill;  // MasterKillEq mask, 0 none
    float filter;   // MasterDjFilter percent, 0 none
  };
  static constexpr Arm kArms[] = {{"dj_kill_low", audio::MasterKillEq::kLow, 0.0f},   {"dj_kill_mid", audio::MasterKillEq::kMid, 0.0f},
                                  {"dj_kill_high", audio::MasterKillEq::kHigh, 0.0f}, {"dj_kill_all", audio::MasterKillEq::kAll, 0.0f},
                                  {"dj_filter_lp50", 0u, -50.0f},                     {"dj_filter_kill_low", audio::MasterKillEq::kLow, -50.0f}};
  for (const Arm& a : kArms) {
    audio::MasterKillEq& kill = *new (raw_kill) audio::MasterKillEq();
    audio::MasterDjFilter& filter = *new (raw_filter) audio::MasterDjFilter();
    bool kill_bypass = false;
    bool filter_bypass = false;
    kill.attach(&kill_bypass);
    filter.attach(&filter_bypass);
    (void)kill.set(static_cast<float>(a.kill));
    (void)filter.set(a.filter, 1u);
    for (unsigned k = 0; k < 12u; ++k) {  // the ramps (5 ms, one block) are over before the first timed block
      fill();
      if (a.kill != 0u) kill.process(l, r, kBlock);
      if (a.filter != 0.0f) filter.process(l, r, kBlock);
    }
    out.stat(a.name, measure(blocks, fill, [&] {
               if (a.kill != 0u) kill.process(l, r, kBlock);
               if (a.filter != 0.0f) filter.process(l, r, kBlock);
             }, nop));
  }
}

void run_delay(const Out& out, std::uint32_t blocks) noexcept {
  if (out.variant == Variant::Sram || (out.alias() && !alias_enabled())) {
    out.na("delay", out.variant == Variant::Sram ? "the ring does not fit SRAM" : "the SDRAM alias is not enabled in this image");
    return;
  }
  if (!stack_room(sizeof(audio::SendDelay) + 1024u)) {
    out.na("delay", "not enough stack");
    return;
  }
  alignas(audio::SendDelay) unsigned char raw[sizeof(audio::SendDelay)];
  audio::SendDelay& d = *new (raw) audio::SendDelay();
  d.attach(scratch_ring(out), kBenchDelayFrames);  // clears 256 KB of SDRAM
  (void)d.set_param(audio::sendfx::delay::Time, 300.0f);
  (void)d.set_param(audio::sendfx::delay::Feedback, 70.0f);
  (void)d.set_param(audio::sendfx::delay::Tone, 60.0f);
  (void)d.set_param(audio::sendfx::delay::Mode, 1.0f);  // STEREO: two different taps
  alignas(8) std::int16_t in[kBlock];
  alignas(8) std::int16_t ol[kBlock];
  alignas(8) std::int16_t orr[kBlock];
  Noise n;
  const Stat st = measure(
      blocks, [&] { for (std::uint32_t k = 0; k < kBlock; ++k) { in[k] = static_cast<std::int16_t>(n.next() / 4); } },
      [&] { d.process(in, ol, orr, kBlock); }, [] {});
  out.stat("delay", st);
}

void run_reverb(const Out& out, std::uint32_t blocks, bool full) noexcept {
  const char* name = full ? "reverb_full" : "reverb";
  if (out.variant == Variant::Sram || (out.alias() && !alias_enabled())) {
    out.na(name, out.variant == Variant::Sram ? "the lines do not fit SRAM" : "the SDRAM alias is not enabled in this image");
    return;
  }
  if (!stack_room(sizeof(audio::SendReverb) + 1024u)) {
    out.na(name, "not enough stack");
    return;
  }
  alignas(audio::SendReverb) unsigned char raw[sizeof(audio::SendReverb)];
  audio::SendReverb& rv = *new (raw) audio::SendReverb();
  rv.attach(scratch_reverb(out), audio::SendReverb::kMemSamples);  // clears 53 KB of SDRAM
  if (full) {
    (void)rv.set_param(audio::sendfx::reverb::Quality, 1.0f);  // the change clears the tail: the return is silent for 13 blocks, the warm-up covers them (idle() would skip the work)
  }
  alignas(8) std::int16_t in[kBlock];
  alignas(8) std::int16_t ol[kBlock];
  alignas(8) std::int16_t orr[kBlock];
  Noise n;
  const Stat st = measure(
      blocks + 16u,  // 16 blocks more, so that the blocks of a clearing tail are not in the timed part
      [&] { for (std::uint32_t k = 0; k < kBlock; ++k) { in[k] = static_cast<std::int16_t>(n.next() / 4); } },
      [&] { rv.process(in, ol, orr, kBlock); }, [] {});
  out.stat(name, st, rv.idle() ? "idle: the effect slept" : nullptr);
}

// `shape`: what the voice is asked to do on top of the plain sample: its instrument filter, the overdrive at 100, the bit depth at 8, or both of the last two (OS-IFX1: the cost of the
// two passes over the voice's block is the difference to `voice`; with both OFF the pass is skipped, which is the `voice` line).
enum class VoiceMode : std::uint8_t { Plain, Filter, Drive, Crush, Both, Rev, Pan };  // Rev (OS-PERF-CPU2): the plain sample played backward (the PLAYBACK layer on track 16), the backward loop of run_dir

void run_voice(const Out& out, std::uint32_t blocks, VoiceMode mode) noexcept {
  const bool filtered = mode == VoiceMode::Filter;
  const char* const name = mode == VoiceMode::Filter ? "voice_f" : (mode == VoiceMode::Drive ? "voice_od" : (mode == VoiceMode::Crush ? "voice_bit" : (mode == VoiceMode::Both ? "voice_odbit" : (mode == VoiceMode::Rev ? "voice_rev" : (mode == VoiceMode::Pan ? "voice_pan" : "voice")))));
  audio::VoicePool& vp = audio_voices();
  if (out.variant == Variant::Sram || (out.variant == Variant::Alias && !alias_enabled())) {
    out.na(name, out.variant == Variant::Sram ? "the sample does not fit SRAM" : "the SDRAM alias is not enabled in this image");
    return;
  }
  // The run renders the LIVE pool from the main loop and cuts track 16 at the end: it only runs when no voice is sounding or fading (a playing song, a pad, the audition), as the tb303 bench
  // does. (Nothing starts a voice behind this test: the transport, the pads and the audition are main-loop work, and the main loop is here.)
  if (vp.voices_in_use() != 0u) {
    out.na(name, "voices are playing: stop the transport");
    return;
  }
  // The longest plain sample: the modern engine, no slices, no wavetable, no granular. It plays at its own pitch (note 60), one voice.
  unsigned slot = audio::kSampleSlots;
  std::uint32_t best = 0;
  for (unsigned s = 0; s < audio::kSampleSlots; ++s) {
    const audio::SampleDesc& d = vp.sample(s);
    if (d.data != nullptr && d.length > best && d.engine == audio::kEngineModern && !d.granular && d.wt_size == 0u && d.slice_count == 0u && !d.beat_slice) {
      best = d.length;
      slot = s;
    }
  }
  if (slot == audio::kSampleSlots) {
    out.na(name, "no plain sample is loaded");
    return;
  }
  const unsigned original = slot;
  // A forced variant: the voice reads the same PCM through the other address, from a copy of the descriptor in the audition's spare slot (put back at the end).
  audio::SampleDesc kept;
  bool borrowed = false;
  if (out.forced() || (mode != VoiceMode::Plain && mode != VoiceMode::Rev)) {
    audio::SampleDesc copy = vp.sample(original);
    const std::uint32_t a = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(copy.data));
    if (filtered) {  // the instrument's filter: a low-pass, cutoff 0.6, resonance 2 (Q 2.7), no modulator
      copy.filter.enable = true;
      copy.filter.wanted = true;
      copy.filter.type = audio::kFilterLow;
      copy.filter.cutoff = 0.6f;
      copy.filter.resonance = 2.0f;
      copy.filter.mod.kind = audio::kFmodNone;
    }
    if (mode == VoiceMode::Drive || mode == VoiceMode::Both) {
      copy.overdrive = 100;  // OS-IFX1: the full drive (gain 16, the hardest clipping)
    }
    if (mode == VoiceMode::Crush || mode == VoiceMode::Both) {
      copy.bit_depth = 8;
    }
    if (mode == VoiceMode::Pan) {  // OS-PERF-CPU2: the instrument's pan LFO, a saw of 8 steps at amount 0.8 (the host bench's): the voice evaluates it every block; the strip's pan chain (Engine::apply_pan_mods) is not in this line
      audio::FilterMod m;
      m.kind = audio::kFmodLfo;
      m.shape = 1;
      m.rate = 8;
      m.amount = 0.8f;
      m.lfo_amount = 0.8f;
      m.lfo = true;
      copy.pan_mod = m;
    }
    const std::uint32_t off = a >= mem::kSdramAliasBase && a < mem::kSdramAliasBase + mem::kSdramSize ? a - mem::kSdramAliasBase
                              : (a >= mem::kSdramBase && a < mem::kSdramBase + mem::kSdramSize ? a - mem::kSdramBase : 0xFFFFFFFFu);
    if (off == 0xFFFFFFFFu) {
      out.na(name, "the sample is not in the SDRAM");
      return;
    }
    if (out.forced()) {
      copy.data = reinterpret_cast<const std::int16_t*>(static_cast<std::uintptr_t>(out.variant == Variant::Alias ? mem::kSdramAliasBase + off : mem::kSdramBase + off));
    }
    {
      HoldRender hold;
      kept = vp.sample(audio::kPreviewSlot);
      vp.set_sample(audio::kPreviewSlot, copy);
    }
    borrowed = true;
    slot = audio::kPreviewSlot;
  }
  constexpr unsigned kTrack = audio::kAuditionTrack;  // track 16, the strip a `mod note` plays on
  const auto reverse_layer = [&](bool on) {  // OS-PERF-CPU2: the PLAYBACK layer of track 16 (the next note starts backward), and its end
    audio::SeqEvent e{};
    e.kind = on ? audio::EventKind::ModSet : audio::EventKind::ModEnd;
    e.track = static_cast<std::uint8_t>(kTrack);
    e.flags = audio::kModGroupReverse;
    e.c = audio::kLayerPerf;
    e.value = on ? 1 : 0;
    HoldRender hold;
    vp.fx_event(kTrack, e);
  };
  if (mode == VoiceMode::Rev) {
    reverse_layer(true);
  }
  bool started = false;
  Peak peak;  // the voice's block, looked at outside the timed part (OS-MODSILENT: a cycle count is not a sound)
  const Stat st = measure(
      blocks,
      [&] {
        if (!vp.track_playing(kTrack)) {
          started = vp.note_on(kTrack, audio::kRootNote, slot, 40) || started;
        }
        vp.begin_block();
      },
      [&] {
        vp.render(0, kBlock);
        vp.end_block();
      },
      [&] { peak.add_q15(vp.feed(kTrack), kBlock); });
  if (mode == VoiceMode::Rev) {
    reverse_layer(false);
  }
  {
    HoldRender hold;
    vp.cut(kTrack);
    if (borrowed) {
      vp.set_sample(audio::kPreviewSlot, kept);
    }
  }
  // The cut leaves a declick residual that the live render fades over the next block or two: wait for it, so that the next `bench voice` finds the pool idle.
  std::uint32_t settled = audio_status().snapshot().renders;
  for (unsigned i = 0; i < 4u && vp.voices_in_use() != 0u; ++i) {
    wait_render(settled);
  }
  if (!started) {
    out.na(name, "the note did not start");
    return;
  }
  char note[40];
  audio::diag::Text t(note, sizeof note);
  t.str("slot ").dec(original + 1u).str(", ").dec(best).str(" samples");
  out.stat_checked(name, st, note, peak, "a silent sample, or a dead voice path");
}

// `voice_st` (OS-STEREO step 1): what a stereo voice's sample loop costs against a mono one, kernel against kernel. Not the pool: the two loops of audio/bench_stereo.h run on their own, on
// planar int16 blocks of the `ui_cache` SDRAM (read as the sample arena is: through the alias, as production does, or the variant's address), one position, the plain voice's interpolation
// (VoicePool::fast_body) and a constant amplitude. A block is 64 frames, the step 1.07 (a fractional step, so that the interpolation works), the position moving on from block to block through
// 32768 frames of each channel. The kernels are in FLASH (the design's placement for the stereo loop). The rows:
//   voice_mono       one block M: M to the output (the plain voice's loop)
//   voice_st         two blocks M and S, one position: L = M + S and R = M - S
// and the line `BENCH voice_st_eq OK|FAIL`: both write what bench_stereo.h's plain reference writes, sample for sample, over 64 blocks. The existing `voice` row (the whole pool, one voice)
// stays the reference for the pool's overhead (it runs the loop from SRAM_L, so `voice` against `voice_mono` also shows what the placement is worth). A `.ramfunc_l` copy of the two kernels
// was tried and does not link: it is +312 B of hot code and the bench image has no room (ld: "hot code exceeds its SRAM_L budget"). The delta voice_st - voice_mono is the cost of the second
// channel; it decides the cap on stereo voices (docs/specs/OS-STEREO.md).
namespace voice_st {

using audio::bench_st::Pos;
constexpr std::uint32_t kFrames = 32768;                         // frames of each channel
constexpr std::uint32_t kSideOffset = kFrames * 2u;              // S follows M, 64 KiB on
constexpr std::uint64_t kInc = (std::uint64_t{1} << 32) + 0x12345678u;  // 1.07 frames a frame
constexpr std::uint32_t kMaxRead = (kBlock * 2u) + 2u;           // frames a block can touch at most: 64 x 2 (a step under 2) plus the neighbour
constexpr std::int32_t kAmp = 10066329;                          // 0.6 in Q24 (the usual voice level)
static_assert(kSideOffset * 2u <= mem::kUiCacheSize && kSideOffset % 8u == 0u, "M and S fit the ui_cache region, S on a doubleword");
static_assert(kInc < (std::uint64_t{2} << 32), "a step under 2 frames: kMaxRead holds");
static_assert(mem::kUiCacheBase % 8u == 0u, "the blocks start on a doubleword");

// Two kernels, in flash. The bodies are audio::bench_st::resample (always_inline).
__attribute__((noinline)) void mono_flash(const std::int16_t* m, std::int16_t* ol, std::uint32_t n, Pos& p) noexcept { audio::bench_st::resample<false>(m, nullptr, ol, nullptr, n, p, kInc, kAmp); }
__attribute__((noinline)) void st_flash(const std::int16_t* m, const std::int16_t* s, std::int16_t* ol, std::int16_t* orr, std::uint32_t n, Pos& p) noexcept {
  audio::bench_st::resample<true>(m, s, ol, orr, n, p, kInc, kAmp);
}

void run(const Out& out, std::uint32_t blocks) noexcept {
  if (out.variant == Variant::Sram) {
    out.na("voice_st", "the blocks are in the SDRAM");
    return;
  }
  if (out.variant == Variant::Alias && !alias_enabled()) {
    out.na("voice_st", "the SDRAM alias is not enabled in this image");
    return;
  }
  if (!stack_room(2048u)) {
    out.na("voice_st", "not enough stack");
    return;
  }
  std::int16_t* const m = reinterpret_cast<std::int16_t*>(scratch_base(out));
  std::int16_t* const s = reinterpret_cast<std::int16_t*>(scratch_base(out) + kSideOffset);
  Noise n;
  for (std::uint32_t i = 0; i < kFrames; ++i) {
    m[i] = static_cast<std::int16_t>(n.next() / 2);  // half scale: M + S and M - S stay inside 16 bits, as (L + R) / 2 and (L - R) / 2 of a 16-bit file do
    s[i] = static_cast<std::int16_t>(n.next() / 2);
  }
  alignas(8) std::int16_t ol[kBlock];
  alignas(8) std::int16_t orr[kBlock];
  Pos p{0u, 0u};
  const auto rewind = [&] {  // the next block stays inside the data: back to the start when it would not (untimed)
    if (p.idx + kMaxRead >= kFrames) {
      p = Pos{0u, 0u};
    }
  };
  const auto nop = [] {};
  const auto row = [&](const char* name, auto&& body, const char* note) {
    p = Pos{0u, 0u};
    out.stat(name, measure(blocks, rewind, body, nop), note);
  };
  row("voice_mono", [&] { mono_flash(m, ol, kBlock, p); }, "M only, flash, 64 frames");
  row("voice_st", [&] { st_flash(m, s, ol, orr, kBlock, p); }, "M and S, flash, 64 frames");
  {
    bool ok = true;
    alignas(8) std::int16_t rl[kBlock];
    alignas(8) std::int16_t rr[kBlock];
    alignas(8) std::int16_t xl[kBlock];
    alignas(8) std::int16_t xr[kBlock];
    for (unsigned round = 0; round < 64u && ok; ++round) {
      const Pos at{(round * 397u) % (kFrames - kMaxRead), (round * 0x9E3779B9u)};
      Pos q = at;
      audio::bench_st::reference(m, nullptr, rl, nullptr, kBlock, at, kInc, kAmp);
      mono_flash(m, xl, kBlock, q);
      ok = std::memcmp(rl, xl, sizeof rl) == 0;
      audio::bench_st::reference(m, s, rl, rr, kBlock, at, kInc, kAmp);
      q = at;
      st_flash(m, s, xl, xr, kBlock, q);
      ok = ok && std::memcmp(rl, xl, sizeof rl) == 0 && std::memcmp(rr, xr, sizeof rr) == 0;
    }
    char line[96];
    audio::diag::Text t(line, sizeof line);
    t.str("BENCH voice_st_eq ").str(ok ? "OK" : "FAIL");
    out.line(line, t.size());
  }
}

}  // namespace voice_st

// `burst` (OS-ODD11 UTAIL): the cost of a pattern step in the scene of the over-budget tail (cpu_mute_ab: 12 voices on the instrument filter, nothing muted), split in four lines on the LIVE pool
// (it REFUSES while something sounds: stop the transport; nothing muted, so that no gate is engaged). The code under test is the one a step runs, not a kernel:
//   burst_on      the 12 note_ons of a step, one per voice (each cut first, untimed)
//   burst_first   the block that follows the notes: begin_block, two spans (the event at frame 32), end_block
//   burst_steady  the same 12 voices a block later: one span
//   burst_4span   the same block cut into four spans (a step with events at four offsets): the per-span cost of the 12 voices, whatever a span pays on top of the stages
// The same arm builds on the baseline 2e40e69 (it calls nothing of the gate), so one binary of each is the A/B: a difference in burst_steady and burst_4span is the voice loop's per-span cost,
// a difference in burst_on the per-note cost. Run it as `bench burst 512` and read min, avg and max.
void run_burst(const Out& out, std::uint32_t blocks) noexcept {
  static constexpr const char* kName = "burst";
  audio::VoicePool& vp = audio_voices();
  if (out.variant != Variant::Normal) {
    out.na(kName, "no variants");
    return;
  }
  if (vp.voices_in_use() != 0u) {
    out.na(kName, "voices are playing: stop the transport");
    return;
  }
  unsigned slot = audio::kSampleSlots;
  std::uint32_t best = 0;
  for (unsigned s = 0; s < audio::kSampleSlots; ++s) {
    const audio::SampleDesc& d = vp.sample(s);
    if (d.data != nullptr && d.length > best && d.engine == audio::kEngineModern && !d.granular && d.wt_size == 0u && d.slice_count == 0u && !d.beat_slice) {
      best = d.length;
      slot = s;
    }
  }
  if (slot == audio::kSampleSlots) {
    out.na(kName, "no plain sample is loaded");
    return;
  }
  // The sample with the instrument filter on (low-pass, cutoff 0.6, resonance 2, no modulator), in the audition's spare slot (put back at the end), as `voice_f` does.
  audio::SampleDesc kept;
  {
    audio::SampleDesc copy = vp.sample(slot);
    copy.filter.enable = true;
    copy.filter.wanted = true;
    copy.filter.type = audio::kFilterLow;
    copy.filter.cutoff = 0.6f;
    copy.filter.resonance = 2.0f;
    copy.filter.mod.kind = audio::kFmodNone;
    HoldRender hold;
    kept = vp.sample(audio::kPreviewSlot);
    vp.set_sample(audio::kPreviewSlot, copy);
  }
  constexpr unsigned kVoices = audio::kMaxVoices;
  const auto cut_all = [&] {
    for (unsigned t = 0; t < kVoices; ++t) {
      vp.cut(t);
    }
  };
  const auto notes = [&] {
    bool all = true;
    for (unsigned t = 0; t < kVoices; ++t) {
      all = vp.note_on(t, audio::kRootNote, audio::kPreviewSlot, 40) && all;
    }
    return all;
  };
  const auto top_up = [&] {  // a voice whose sample ended starts again: the steady lines always have the 12
    for (unsigned t = 0; t < kVoices; ++t) {
      if (!vp.track_playing(t)) {
        (void)vp.note_on(t, audio::kRootNote, audio::kPreviewSlot, 40);
      }
    }
  };
  bool started = true;
  Peak peak;
  const Stat on = measure(blocks, [&] { cut_all(); }, [&] { started = notes() && started; }, [&] {});
  const Stat first = measure(
      blocks, [&] { cut_all(); started = notes() && started; vp.begin_block(); },
      [&] {
        vp.render(0, kBlock / 2u);
        vp.render(kBlock / 2u, kBlock);
        vp.end_block();
      },
      [&] { peak.add_q15(vp.feed(0), kBlock); });
  const Stat steady = measure(blocks, [&] { top_up(); vp.begin_block(); }, [&] {
    vp.render(0, kBlock);
    vp.end_block();
  }, [&] {});
  const Stat four = measure(blocks, [&] { top_up(); vp.begin_block(); }, [&] {
    for (unsigned k = 0; k < 4u; ++k) {
      vp.render(k * (kBlock / 4u), (k + 1u) * (kBlock / 4u));
    }
    vp.end_block();
  }, [&] {});
  {
    HoldRender hold;
    cut_all();
    vp.set_sample(audio::kPreviewSlot, kept);
  }
  std::uint32_t settled = audio_status().snapshot().renders;
  for (unsigned i = 0; i < 4u && vp.voices_in_use() != 0u; ++i) {  // the cuts leave a declick residual: the next run finds the pool idle
    wait_render(settled);
  }
  if (!started) {
    out.na(kName, "a note did not start");
    return;
  }
  out.stat("burst_on", on, "12 note_ons");
  out.stat_checked("burst_first", first, "12 voices, 2 spans", peak, "a silent sample, or a dead voice path");
  out.stat("burst_steady", steady, "12 voices, 1 span");
  out.stat("burst_4span", four, "12 voices, 4 spans");
}

bool name_has(const char* s, const char* what) noexcept {
  for (; *s != '\0'; ++s) {
    const char* a = s;
    const char* b = what;
    while (*a != '\0' && *a == *b) {
      ++a;
      ++b;
    }
    if (*b == '\0') {
      return true;
    }
  }
  return false;
}

// `frames` per call of the module's render(): 64 is the engine's block, 128 the largest the ABI allows (O3M_BLOCK_MAX); the module is called with the render held off, once, whatever the count.
void run_tb303(const Out& out, std::uint32_t blocks, std::uint32_t frames) noexcept {
  const char* const name = frames == kBlock ? "tb303" : "tb303_128";
  audio::ModuleVoices* mv = appimg::module_voices();
  if (mv == nullptr) {
    out.na(name, "this image has no sound");
    return;
  }
  audio::ModuleVoices::Info info{};
  bool found = false;
  for (unsigned i = 0; i < audio::kMaxModuleInstances && !found; ++i) {
    found = mv->info(i, info) && info.mod != nullptr && info.mod->name != nullptr && name_has(info.mod->name, "303");
  }
  if (!found) {
    out.na(name, "no TB-303 instance is bound (mod load tb303; mod bind 1 tb303)");
    return;
  }
  void* const live = mv->state_of(info.slot);
  const o3m_module* const mod = info.mod;
  if (live == nullptr || mod == nullptr || mod->render == nullptr || mod->init == nullptr || mod->shutdown == nullptr) {
    out.na(name, "the instance has no state");
    return;
  }
  static_assert(O3M_BLOCK_MAX == 128, "the bench buffer is the largest call the ABI allows");
  alignas(16) float buf[O3M_BLOCK_MAX];
  // The run uses a PRIVATE instance, freshly init()ed (OS-MODSILENT): the same note at the same oscillator phase, filter state and defaults every time, so that the peak is a figure that can be compared
  // between runs and builds, and the live instance (its parameters, its held note) is never touched. Where the copy lives: as the variant says (SRAM_U = the stack, or SDRAM through the alias or
  // 0x7000_0000), and for the production run where the live instance's state is (SRAM_U since OS-PERF3; the SDRAM arena when the 4 KiB state arena was full), through the view the core works in.
  alignas(16) unsigned char sram_copy[512];
  if (mod->state_bytes > sizeof sram_copy || mod->state_bytes > 1024u) {
    out.na(name, "the state is too big to copy");
    return;
  }
  const std::uint32_t live_addr = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(live));
  const bool live_in_sram = live_addr >= mem::kSramUBase && live_addr < mem::kSramUBase + mem::kSramUSize;
  const Variant where = out.variant != Variant::Normal ? out.variant : (live_in_sram ? Variant::Sram : (mem::kCoreUsesAlias ? Variant::Alias : Variant::Direct));
  void* state = sram_copy;
  if (where != Variant::Sram) {
    if (where == Variant::Alias && !alias_enabled()) {
      out.na(name, "the SDRAM alias is not enabled in this image");
      return;
    }
    const std::uint32_t base = where == Variant::Alias ? mem::kSdramAliasBase : mem::kSdramBase;
    state = reinterpret_cast<void*>(static_cast<std::uintptr_t>(base + (mem::kUiCacheBase - mem::kSdramBase) + kBenchStateOffset));
  }
  std::memset(state, 0, mod->state_bytes);
  if (mod->init(state, &mv->host()) != 0) {  // (main loop: nothing else touches this block; init() calls no render-side state)
    out.na(name, "init() refused");
    return;
  }
  bool sounding = false;
  Peak peak;  // the module's float block, looked at outside the timed part (OS-MODSILENT: the bench used to time a render and never look at what it made)
  const Stat st = measure(
      blocks,
      [&] {
        if (!sounding) {
          mod->note_on(state, 45u, 110u, 0u);  // A1, accented: the heaviest path of the voice (the accent envelope runs)
          sounding = true;
        }
      },
      [&] { sounding = mod->render(state, nullptr, buf, frames) != 0u; }, [&] { peak.add_float(buf, frames); });
  mod->shutdown(state);
  char label[48];  // the module's name, the frames of a call and the cost of a frame (the minimum over the frames: tb303 and tb303_128 read against each other)
  audio::diag::Text lt(label, sizeof label);
  lt.str(info.mod->name).str(", ").dec(frames).str(" frames, ").dec(st.min() / frames).str(" cycles/frame");
  out.stat_checked(name, st, label, peak);
}

// ---- in situ: the render's own stage figures -------------------------------------------------------------------------------------------
void run_stages(const Out& out, std::uint32_t blocks, bool only_strips) noexcept {
  Stat total;
  Stat stage[audio::kStageCount];
  std::uint32_t renders = audio_status().snapshot().renders;
  std::uint32_t last_seen = renders;
  for (std::uint32_t i = 0; i < blocks; ++i) {
    wait_render(renders);
    if (!audio_running()) {
      break;
    }
    const audio::StatusSnapshot s = audio_status().snapshot();
    if (s.renders == last_seen) {
      continue;
    }
    last_seen = s.renders;
    total.add(s.render_last);
    for (unsigned k = 0; k < audio::kStageCount; ++k) {
      stage[k].add(s.stage_last[k]);
    }
  }
  static constexpr const char* kNames[audio::kStageCount] = {"stage_input", "stage_drain", "stage_render", "stage_strips", "stage_buses", "stage_master", "stage_convert", "stage_taps", "stage_commit"};
  if (only_strips) {
    out.stat("strips", stage[hal::audio::kStageStrips], "in situ");
    return;
  }
  for (unsigned k = 0; k < audio::kStageCount; ++k) {
    out.stat(kNames[k], stage[k], "in situ");
  }
  out.stat("render_total", total, "in situ");
}

// ---- the graph's kernels ----------------------------------------------------------------------------------------------------------------
// Each SIMD kernel of the render on 64 samples of noise, cached and idle (the minimum): where the always-on cost of an empty block goes.
void run_kernels(const Out& out, std::uint32_t blocks) noexcept {
  namespace k = audio::kernel;
  alignas(8) std::int16_t a[kBlock];
  alignas(8) std::int16_t b[kBlock];
  alignas(8) std::int16_t c[kBlock];
  alignas(8) std::int16_t io[2 * kBlock];
  alignas(8) std::int32_t acc_l[kBlock] = {};
  alignas(8) std::int32_t acc_r[kBlock] = {};
  alignas(8) std::int32_t acc_c[kBlock] = {};
  alignas(8) float fl[kBlock];
  alignas(8) float fr[kBlock];
  Noise n;
  for (std::uint32_t i = 0; i < kBlock; ++i) {
    a[i] = static_cast<std::int16_t>(n.next() / 2);
    b[i] = static_cast<std::int16_t>(n.next() / 2);
    c[i] = 0;
    fl[i] = static_cast<float>(n.next()) / 32768.0f;
    fr[i] = static_cast<float>(n.next()) / 32768.0f;
  }
  for (std::uint32_t i = 0; i < 2 * kBlock; ++i) {
    io[i] = static_cast<std::int16_t>(n.next() / 2);
  }
  k::MeterAcc meter{0, 0};
  const k::StripLane lane{c, {50000, 51000}, {60000, 61000}, {30000, 31000}};
  const k::StripLane lane2{b, {40000, 41000}, {65536, 65536}, {65536, 65536}};
  const auto nop = [] {};
  const auto run = [&](const char* name, auto&& body) { out.stat(name, measure(blocks, nop, body, nop)); };
  run("k_copy128B", [&] { std::memcpy(c, a, sizeof c); });
  run("k_gain", [&] { k::gain(c, a, kBlock, 40000, 41000); });
  run("k_mac", [&] { k::mac(acc_l, a, kBlock, 40000, 41000); });
  run("k_gain_pan", [&] { k::gain_pan(lane, acc_l, acc_r, kBlock); });
  run("k_gain_mac", [&] { k::gain_mac(lane2, acc_c, kBlock); });
  run("k_gain_pan2", [&] { k::gain_pan2(lane, lane2, acc_l, acc_r, kBlock); });
  run("k_sat_clear", [&] { k::sat_clear(c, acc_l, kBlock); });
  run("k_sat_clear_main", [&] { k::sat_clear_main(a, b, acc_l, acc_r, acc_c, kBlock); });
  run("k_acc_to_float_main", [&] { k::acc_to_float_main(fl, fr, acc_l, acc_r, acc_c, kBlock); });
  run("k_mono_mix", [&] { k::mono_mix(c, a, b, kBlock); });
  run("k_deinterleave2", [&] { k::deinterleave2(a, b, io, kBlock); });
  run("k_interleave2", [&] { k::interleave2(io, a, b, kBlock); });
  run("k_meter", [&] { k::meter(a, kBlock, meter); });
  run("k_q15_to_float", [&] { k::q15_to_float(fl, a, kBlock); });
  run("k_float_to_q15", [&] { k::float_to_q15(c, fl, kBlock); });
}

// ---- OS-CPUOPT1: the loop player and the SVF, old reference against production ----------------------------------------------------------------
// `ref_*` are the implementations of dev 604d721 copied as they were (the lambda and all): the A of the A/B. The B is the production code of this image, where the commit that changed it adds
// a line. Each pair also runs on the same random input and compares the output: `BENCH cpuopt_eq_<name> OK` or `FAIL ...` (the host cannot prove the SVF: it has no VMLA to fuse).
namespace cpuopt {

// The loop player's block as it was: LoopPlayer::run, loop_player.cpp (the take, then the held last frame, then silence), g in Q16 moving by `step` for `ramp` frames, then constant.
__attribute__((noinline)) std::uint32_t ref_loop_block(std::int16_t* out, const std::int16_t* p, std::uint32_t len, std::uint32_t pos, std::uint32_t n, std::int32_t g, std::int32_t dgain,
                                                       std::uint32_t ramp) noexcept {
  using audio::simd::NativeOps;
  constexpr std::uint32_t kHoldFrames = 2;
  std::int16_t* o = out;
  const auto body = [&](std::uint32_t count, std::int32_t step) {
    std::uint32_t whole = pos < len ? len - pos : 0u;
    whole = whole < count ? whole : count;
    count -= whole;
    for (; whole != 0u; --whole, o += 2, g += step) {
      const std::int32_t v = (static_cast<std::int32_t>(p[pos]) * g) >> 16;
      ++pos;
      o[0] = static_cast<std::int16_t>(NativeOps::ssat16(o[0] + v));
      o[1] = static_cast<std::int16_t>(NativeOps::ssat16(o[1] + v));
    }
    std::uint32_t held = pos < len + kHoldFrames ? len + kHoldFrames - pos : 0u;
    held = held < count ? held : count;
    count -= held;
    for (; held != 0u; --held, o += 2, g += step) {
      const std::int32_t v = (static_cast<std::int32_t>(p[len - 1u]) * g) >> 16;
      ++pos;
      o[0] = static_cast<std::int16_t>(NativeOps::ssat16(o[0] + v));
      o[1] = static_cast<std::int16_t>(NativeOps::ssat16(o[1] + v));
    }
    o += 2u * count;
    g += step * static_cast<std::int32_t>(count);
  };
  const std::uint32_t ramped = n < ramp ? n : ramp;
  body(ramped, dgain);
  body(n - ramped, 0);
  return pos;
}

// The SVF's sample loop as it was (svf.h of dev 604d721, plain products the compiler may fuse into VMLA): the A of C5.
template <std::uint8_t Type>
__attribute__((noinline)) void ref_svf(audio::Svf& s, std::int16_t* io, std::uint32_t n) noexcept {
  const float f = s.f;
  const float d = s.d;
  float lp = s.lp;
  float bp = s.bp;
  float prev = s.prev;
  for (std::uint32_t k = 0; k < n; ++k) {
    const float x = static_cast<float>(io[k]);
    lp += f * bp;
    float hp = 0.5f * (x + prev) - lp - d * bp;
    prev = x;
    bp += f * hp;
    const float lp0 = lp;
    const float bp0 = bp;
    const float hp0 = hp;
    lp += f * bp;
    hp = x - lp - d * bp;
    bp += f * hp;
    const float y = Type == audio::kFilterLow ? lp + lp0 : (Type == audio::kFilterHigh ? hp + hp0 : bp + bp0);
    io[k] = audio::svf_out(0.5f * y);
  }
  s.lp = lp;
  s.bp = bp;
  s.prev = prev;
}

// Block-sized random take for the loop: 64 KiB of the unused ui_cache region (through the alias as the arena is read), so every measured block reads lines the cache does not hold.
constexpr std::uint32_t kTakeFrames = 32768;

void run(const Out& out, std::uint32_t blocks) noexcept {
  if (!stack_room(2048u)) {
    out.na("cpuopt", "not enough stack");
    return;
  }
  std::int16_t* take = reinterpret_cast<std::int16_t*>(scratch_base(out));
  Noise n;
  for (std::uint32_t i = 0; i < kTakeFrames; ++i) {
    take[i] = static_cast<std::int16_t>(n.next());
  }
  alignas(8) std::int16_t tx[2 * kBlock];
  alignas(8) std::int16_t src[kBlock];
  for (std::uint32_t i = 0; i < kBlock; ++i) {
    src[i] = static_cast<std::int16_t>(n.next());
  }
  std::uint32_t pos = 0;
  const auto fill_tx = [&] {
    for (std::uint32_t i = 0; i < 2 * kBlock; ++i) {
      tx[i] = static_cast<std::int16_t>(n.next() / 2);
    }
  };
  const auto step_pos = [&] { pos = (pos + kBlock) % (kTakeFrames - kBlock); };
  const auto nop = [] {};
  // the loop player's block, constant gain 0.6 (the usual) and a 32-frame ramp (a stop, a start)
  out.stat("loop_ref", measure(blocks, [&] { fill_tx(); step_pos(); }, [&] { (void)ref_loop_block(tx, take, kTakeFrames, pos, kBlock, 39000, 0, 0u); }, nop), "constant gain, halfword reads");
  out.stat("loop_ref_ramp", measure(blocks, [&] { fill_tx(); step_pos(); }, [&] { (void)ref_loop_block(tx, take, kTakeFrames, pos, kBlock, 0, 1200, 32u); }, nop), "32 frames of ramp");
  // the loop player's block with the new inner loop, and the proof on the unit that it equals the old one (random TX content, both gains)
  out.stat("loop_new", measure(blocks, [&] { fill_tx(); step_pos(); }, [&] { (void)audio::loop_mix::take(tx, take + pos, kBlock, 39000, 0); }, nop), "constant gain, words, qadd16");
  out.stat("loop_new_ramp", measure(blocks, [&] { fill_tx(); step_pos(); }, [&] {
    std::int32_t g = audio::loop_mix::take(tx, take + pos, 32, 0, 1200);
    (void)audio::loop_mix::take(tx + 64, take + pos + 32, 32, g, 0);
  }, nop), "32 frames of ramp");
  {
    bool ok = true;
    alignas(8) std::int16_t a[2 * kBlock];
    alignas(8) std::int16_t b[2 * kBlock];
    for (unsigned round = 0; round < 64u && ok; ++round) {
      for (std::uint32_t i = 0; i < 2 * kBlock; ++i) {
        a[i] = b[i] = static_cast<std::int16_t>(n.next());
      }
      const std::uint32_t at = (round * 37u) % (kTakeFrames - kBlock);
      const std::int32_t g0 = static_cast<std::int32_t>((round * 1021u) % 65537u);
      const bool ramp = (round & 1u) != 0u;
      (void)ref_loop_block(a, take, kTakeFrames, at, kBlock, ramp ? 0 : g0, ramp ? 1000 : 0, ramp ? 64u : 0u);
      std::int16_t* o = b;
      std::int32_t g = ramp ? 0 : g0;
      g = audio::loop_mix::take(o, take + at, ramp ? 64u : kBlock, g, ramp ? 1000 : 0);
      if (ramp) {
        (void)audio::loop_mix::take(o + 128, take + at + 64, 0, g, 0);
      }
      ok = std::memcmp(a, b, sizeof a) == 0;
    }
    char line[96];
    audio::diag::Text t(line, sizeof line);
    t.str("BENCH cpuopt_eq_loop ").str(ok ? "OK" : "FAIL");
    out.line(line, t.size());
  }
  // the SVF over 64 samples, the three types: the production svf_run (C5) against the old loop, cycles and a bit-for-bit check of what they make (output and state) on noise
  audio::Svf s;
  audio::Svf r;
  audio::svf_set(s, 0.4f, 2.0f);
  audio::svf_set(r, 0.4f, 2.0f);
  out.stat("svf_ref_low", measure(blocks, [&] { std::memcpy(tx, src, sizeof src); }, [&] { ref_svf<audio::kFilterLow>(r, tx, kBlock); }, nop), "plain products, 64 samples, resonance 2");
  out.stat("svf_low", measure(blocks, [&] { std::memcpy(tx, src, sizeof src); }, [&] { audio::svf_run<audio::kFilterLow>(s, tx, kBlock); }, nop), "64 samples, resonance 2");
  out.stat("svf_high", measure(blocks, [&] { std::memcpy(tx, src, sizeof src); }, [&] { audio::svf_run<audio::kFilterHigh>(s, tx, kBlock); }, nop), "64 samples, resonance 2");
  out.stat("svf_band", measure(blocks, [&] { std::memcpy(tx, src, sizeof src); }, [&] { audio::svf_run<audio::kFilterBand>(s, tx, kBlock); }, nop), "64 samples, resonance 2");
  {
    bool ok = true;
    for (unsigned type = 0; type < 3u && ok; ++type) {
      for (unsigned round = 0; round < 40u && ok; ++round) {
        audio::Svf x;
        audio::Svf y;
        const float cut = static_cast<float>((round * 53u) % 100u) * 0.01f;
        const float res = static_cast<float>(round % 5u);
        audio::svf_set(x, cut, res);
        audio::svf_set(y, cut, res);
        alignas(8) std::int16_t ia[kBlock];
        alignas(8) std::int16_t ib[kBlock];
        for (unsigned blk = 0; blk < 6u && ok; ++blk) {
          for (std::uint32_t i = 0; i < kBlock; ++i) {
            ia[i] = ib[i] = static_cast<std::int16_t>(round % 7u == 0u ? ((n.next() & 1) ? 32767 : -32768) : n.next());
          }
          if (type == audio::kFilterLow) {
            ref_svf<audio::kFilterLow>(x, ia, kBlock);
            audio::svf_run<audio::kFilterLow>(y, ib, kBlock);
          } else if (type == audio::kFilterHigh) {
            ref_svf<audio::kFilterHigh>(x, ia, kBlock);
            audio::svf_run<audio::kFilterHigh>(y, ib, kBlock);
          } else {
            ref_svf<audio::kFilterBand>(x, ia, kBlock);
            audio::svf_run<audio::kFilterBand>(y, ib, kBlock);
          }
          ok = std::memcmp(ia, ib, sizeof ia) == 0 && std::memcmp(&x.lp, &y.lp, sizeof(float)) == 0 && std::memcmp(&x.bp, &y.bp, sizeof(float)) == 0 &&
               std::memcmp(&x.prev, &y.prev, sizeof(float)) == 0;
        }
      }
    }
    char line[96];
    audio::diag::Text t(line, sizeof line);
    t.str("BENCH cpuopt_eq_svf ").str(ok ? "OK" : "FAIL");
    out.line(line, t.size());
  }
}

}  // namespace cpuopt

// ---- OS-LOOPER L2b: the overdub kernels (audio/loop_overdub.h) on SDRAM buffers the size of a long take, cache-cold ---------------------------------------------------------------------
// The loop, its undo buffer and the bitmap lie in the unused ui_cache region (96 K frames each: a take of about 2.2 s), and every measured block steps 64 frames on, as `loop_new` does, so no block
// is read twice in a row. rmw_fb100 is the FB = 100 case without multiplies, rmw_fb50 the general one; `_odd` starts the loop at an odd frame (a leading halfword, then the input is read as two
// halfwords). cow_copy is the first-write copy of one 64-frame block, restore_step one dirty block put back (one copy, a few bitmap reads). `looper_eq_*` lines compare the production code with the
// scalar definition on the unit (the M4's QADD16 / SMLAWB path, which the host test only models).
namespace looper {

constexpr std::uint32_t kFrames = 96u * 1024u;
constexpr std::uint32_t kUndoOffset = kFrames * 2u;                   // bytes
constexpr std::uint32_t kBitmapOffset = 2u * kFrames * 2u;            // bytes
constexpr std::uint32_t kBitmapHalfwords = kFrames / 64u / 16u;
static_assert(kBitmapOffset + kBitmapHalfwords * 2u <= mem::kUiCacheSize, "the looper bench's loop, undo buffer and bitmap fit the ui_cache region");

std::int32_t ref_sat(std::int32_t x) noexcept { return x > 32767 ? 32767 : (x < -32768 ? -32768 : x); }

void run(const Out& out, std::uint32_t blocks) noexcept {
  if (!stack_room(1024u)) {
    out.na("looper", "not enough stack");
    return;
  }
  std::int16_t* loop = reinterpret_cast<std::int16_t*>(scratch_base(out));
  std::int16_t* undo = reinterpret_cast<std::int16_t*>(scratch_base(out) + kUndoOffset);
  std::uint16_t* bitmap = reinterpret_cast<std::uint16_t*>(scratch_base(out) + kBitmapOffset);
  Noise n;
  for (std::uint32_t i = 0; i < kFrames; ++i) {
    loop[i] = static_cast<std::int16_t>(n.next());
    undo[i] = static_cast<std::int16_t>(n.next());
  }
  alignas(8) std::int16_t in[kBlock + 2u];  // (+2: the _inodd arms read it from frame 1)
  for (std::uint32_t i = 0; i < kBlock + 2u; ++i) {
    in[i] = static_cast<std::int16_t>(n.next());
  }
  std::uint32_t pos = 0;
  const auto step_pos = [&] { pos = (pos + kBlock) % (kFrames - 2u * kBlock); };
  const auto nop = [] {};
  constexpr std::int32_t kHalf = 32768;
  out.stat("rmw_fb100", measure(blocks, step_pos, [&] { audio::loop_ov::loop_rmw_add(loop + pos, in, kBlock); }, nop), "64 frames, aligned, no multiplies");
  out.stat("rmw_fb100_odd", measure(blocks, step_pos, [&] { audio::loop_ov::loop_rmw_add(loop + pos + 1, in, kBlock); }, nop), "64 frames, odd start");
  out.stat("rmw_fb50", measure(blocks, step_pos, [&] { audio::loop_ov::loop_rmw(loop + pos, in, kBlock, kHalf); }, nop), "64 frames, aligned, feedback 50");
  out.stat("rmw_fb50_odd", measure(blocks, step_pos, [&] { audio::loop_ov::loop_rmw(loop + pos + 1, in, kBlock, kHalf); }, nop), "64 frames, odd start");
  // The session writes a span that starts anywhere in the block (after a restart inside it), so the input can be on an odd frame while the loop is aligned, or both: the arms the aligned and `_odd` ones leave out.
  out.stat("rmw_fb100_inodd", measure(blocks, step_pos, [&] { audio::loop_ov::loop_rmw_add(loop + pos, in + 1, kBlock); }, nop), "64 frames, loop aligned, input from an odd frame");
  out.stat("rmw_fb50_inodd", measure(blocks, step_pos, [&] { audio::loop_ov::loop_rmw(loop + pos, in + 1, kBlock, kHalf); }, nop), "64 frames, loop aligned, input from an odd frame, feedback 50");
  out.stat("rmw_fb50_bothodd", measure(blocks, step_pos, [&] { audio::loop_ov::loop_rmw(loop + pos + 1, in + 1, kBlock, kHalf); }, nop), "64 frames, both from an odd frame, feedback 50");
  out.stat("cow_copy", measure(blocks, step_pos, [&] { audio::loop_ov::loop_block_copy(undo + pos, loop + pos, kBlock); }, nop), "one 128 B block, loop to undo");
  // the restore: one dirty block per call (the bit is set outside the timing)
  std::memset(bitmap, 0, kBitmapHalfwords * 2u);
  std::uint32_t blk = 0;
  out.stat("restore_step", measure(blocks, [&] {
    step_pos();
    blk = pos / kBlock;
    bitmap[blk >> 4] = static_cast<std::uint16_t>(bitmap[blk >> 4] | (1u << (blk & 15u)));
  }, [&] { (void)audio::loop_ov::loop_restore_step(loop, undo, bitmap, kFrames, blk, 1, 1); }, nop), "one dirty block put back, undo to loop");
  // the restore's scan: a clean bitmap (the worst case of a step: nothing to copy, every word read) with the session's real budgets (kRestoreStepCopies, kRestoreStepWords), cache-cold
  std::memset(bitmap, 0, kBitmapHalfwords * 2u);
  std::uint32_t cursor = 0;
  out.stat("restore_scan", measure(blocks, [&] {
    cursor = (cursor + audio::loop_ov::kRestoreStepWords * 16u * 4u) % (kFrames / kBlock - audio::loop_ov::kRestoreStepWords * 16u);  // spread over the bitmap
  }, [&] { (void)audio::loop_ov::loop_restore_step(loop, undo, bitmap, kFrames, cursor, audio::loop_ov::kRestoreStepCopies, audio::loop_ov::kRestoreStepWords); }, nop), "clean bitmap, kRestoreStepWords halfwords read");
  // the same kernels against their definition, on stack buffers (random data and both rails, both parities of the loop)
  {
    bool ok = true;
    alignas(8) std::int16_t a[kBlock + 4];
    alignas(8) std::int16_t b[kBlock + 4];
    for (unsigned round = 0; round < 96u && ok; ++round) {
      const unsigned skew = round & 1u;
      const std::int32_t fb = (round & 2u) != 0u ? static_cast<std::int32_t>((round * 4099u) % 65537u) : ((round & 4u) != 0u ? 65536 : 0);
      for (std::uint32_t i = 0; i < kBlock + 4; ++i) {
        const std::int16_t rail = (n.next() & 1) ? 32767 : -32768;
        a[i] = b[i] = static_cast<std::int16_t>(round % 7u == 0u ? rail : n.next());
      }
      for (std::uint32_t i = 0; i < kBlock; ++i) {
        in[i] = static_cast<std::int16_t>(round % 5u == 0u ? ((n.next() & 1) ? 32767 : -32768) : n.next());
      }
      const std::uint32_t len = kBlock - (round % 3u);  // 62, 63 and 64 frames: an odd length too
      for (std::uint32_t i = 0; i < len; ++i) {
        b[skew + i] = static_cast<std::int16_t>(ref_sat(((static_cast<std::int32_t>(b[skew + i]) * fb) >> 16) + in[i]));
      }
      if (fb == 65536 && (round & 8u) != 0u) {
        audio::loop_ov::loop_rmw_add(a + skew, in, len);
      } else {
        audio::loop_ov::loop_rmw(a + skew, in, len, fb);
      }
      ok = std::memcmp(a, b, sizeof a) == 0;
    }
    char line[96];
    audio::diag::Text t(line, sizeof line);
    t.str("BENCH looper_eq_rmw ").str(ok ? "OK" : "FAIL");
    out.line(line, t.size());
  }
  {
    // restore over a small loop of 3 blocks and a half: blocks 0, 2 and 3 dirty (3 is the partial one)
    alignas(8) std::int16_t l[224];
    alignas(8) std::int16_t u[224];
    std::uint16_t bm[2] = {0x000D, 0};
    for (std::uint32_t i = 0; i < 224u; ++i) {
      l[i] = static_cast<std::int16_t>(n.next());
      u[i] = static_cast<std::int16_t>(n.next());
    }
    std::int16_t want[224];
    std::memcpy(want, l, sizeof want);
    std::memcpy(want, u, 64u * 2u);
    std::memcpy(want + 128, u + 128, 96u * 2u);
    std::uint32_t c = audio::loop_ov::loop_restore_step(l, u, bm, 224, 0, 2, 4);
    c = audio::loop_ov::loop_restore_step(l, u, bm, 224, c, 2, 4);
    const bool ok = c == 4u && bm[0] == 0 && std::memcmp(l, want, sizeof l) == 0;
    char line[96];
    audio::diag::Text t(line, sizeof line);
    t.str("BENCH looper_eq_restore ").str(ok ? "OK" : "FAIL");
    out.line(line, t.size());
  }
}

}  // namespace looper

// ---- the slowest render ----------------------------------------------------------------------------------------------------------------------
void run_worst(const Out& out) noexcept {
  const audio::Pipeline::WorstRender w = audio_worst_render();
  static constexpr const char* kNames[audio::kStageCount] = {"input", "drain", "render", "strips", "buses", "master", "convert", "taps", "commit"};
  char buf[200];
  out.line(buf, format_worst(w.total, w.stage, audio::kStageCount, kNames, buf, sizeof buf));
}

// ---- memory characterisation -------------------------------------------------------------------------------------------------------------
// A pass of n 32-bit loads, `stride` bytes apart, timed with the cycle counter. In SRAM_L so that the loop's own fetches cost the same in every row (no flash, no cache, zero wait).
// The loop overhead (a load, an add, a counter and a branch) is in every figure; the SRAM_U row is the floor.
O3OS_RAMFUNC_L std::uint32_t load_pass(const volatile std::uint32_t* p, std::uint32_t stride_words, std::uint32_t n) noexcept {
  const std::uint32_t t0 = regs::dwt_cyccnt();
  std::uint32_t acc = 0;
  for (std::uint32_t i = 0; i < n; ++i) {
    acc += *p;
    p += stride_words;
  }
  const std::uint32_t t1 = regs::dwt_cyccnt();
  asm volatile("" : : "r"(acc));
  return t1 - t0;
}

// The warm figure of a region: the best of a few passes over the same words (the first pass fills a cache, if one holds the region), x100 per load.
std::uint32_t load_cost_x100(std::uintptr_t base, std::uint32_t stride_bytes, std::uint32_t n) noexcept {
  std::uint32_t best = 0xFFFFFFFFu;
  for (unsigned rep = 0; rep < 6u; ++rep) {
    const std::uint32_t c = load_pass(reinterpret_cast<const volatile std::uint32_t*>(base), stride_bytes / 4u, n);
    best = c < best ? c : best;
  }
  return (best * 100u + n / 2u) / n;
}

void run_mem(const Out& out) noexcept {
  struct Region {
    const char* name;
    std::uintptr_t base;
    std::uint32_t far_stride;  // bytes between loads of the second figure
    std::uint32_t far_n;
  };
  // SRAM_U's words are the unused stretch behind .bss (the stack region below its floor: never written here, only read). The stride of the far figure opens a different SDRAM row each load
  // (a row is 1-2 KiB), and 128 loads of it span 260 KB of the unused ui_cache region.
  const Region regions[] = {
      {"sram_u", mem::kSramUBase + 0x20000u, 100u, 128u},  // (128 x 100 B: 12.8 KB, inside the free stretch between .bss and the stack)
      {"sram_l", mem::kSramLBase, 100u, 128u},
      {"flash", 0x00010000u, 1028u, 128u},
      {"sdram", mem::kUiCacheBase, 2052u, 128u},
      {"sdram_alias", mem::sdram_alias(mem::kUiCacheBase), 2052u, 128u},  // through 0x0800_0000: the code cache can hold it (a row only when the controller answers there)
  };
  for (const Region& r : regions) {
    if (r.base < mem::kSramLBase && r.base >= 0x01000000u && !alias_enabled()) {
      continue;
    }
    const std::uint32_t seq = load_cost_x100(r.base, 4u, 256u);
    const std::uint32_t far = load_cost_x100(r.base, r.far_stride, r.far_n);
    char buf[96];
    out.line(buf, format_mem_row(r.name, seq, far, buf, sizeof buf));
  }
}

// ---- coherence of the cached alias (the single-alias rule on the unit) --------------------------------------------------------------------
// `diag cache check`: main loop only, bounded (a few thousand accesses to a 64 KB scratch stretch and one pass over the loaded PCM), the lines and verdicts are cache_check.h's. The
// scratch is the last 64 KB of the ui_cache region, which nothing else uses; every step puts back what it changed.
constexpr std::uint32_t kCheckOffset = 448u * mem::kKiB;  // the last 64 KB of the ui_cache region
constexpr std::uint32_t kCheckWords = 16384u;
constexpr std::uint32_t kControlWords = 256u;             // 16 lines of 16 B... x4: 1 KB, resident in the 8 KiB cache once read
static_assert(kCheckOffset + kCheckWords * 4u <= mem::kUiCacheSize && kCheckOffset >= kBenchStateOffset + 1024u, "the check's scratch is clear of the bench's");

// The write-hit step's block: 128 lines (2 KiB: 128 of the cache's 256 sets, one line each), right behind the control's words, inside the scratch.
constexpr std::uint32_t kHitLines = 128u;
constexpr std::uint32_t kHitWords = kHitLines * 4u;
constexpr std::uint32_t kHitFirstWord = kControlWords;
static_assert(kHitFirstWord + kHitWords <= kCheckWords, "the hit step's block is inside the scratch");

inline std::uint32_t fold(std::uint32_t acc, std::uint32_t w) noexcept { return (acc ^ w) * 16777619u + (acc >> 27); }

std::uint32_t sum_words(std::uintptr_t base, std::uint32_t words) noexcept {
  const volatile std::uint32_t* p = reinterpret_cast<const volatile std::uint32_t*>(base);
  std::uint32_t acc = 2166136261u;
  for (std::uint32_t i = 0; i < words; ++i) {
    acc = fold(acc, p[i]);
  }
  return acc;
}

inline void read_line(const volatile std::uint32_t* words, std::uint32_t line, std::uint32_t got[4]) noexcept {
  for (unsigned w = 0; w < 4; ++w) {
    got[w] = words[line * 4u + w];
  }
}

// The stores of line `line` (cache_check.h kHitPlan), each with the access width the plan says: STR, STRH and STRB. Volatile accesses of exact width on purpose (the bus sees what the
// program writes; the compiler may not merge them, and the loads that follow are volatile too, so the order is the program's).
inline void store_line(std::uintptr_t line_address, std::uint32_t line) noexcept {
  for (unsigned op = 0; op < kHitPlanCount; ++op) {
    const std::uintptr_t at = line_address + kHitPlan[op].offset;
    const std::uint32_t v = hit_store_value(line, op);
    if (kHitPlan[op].width == 4u) {
      *reinterpret_cast<volatile std::uint32_t*>(at) = v;
    } else if (kHitPlan[op].width == 2u) {
      *reinterpret_cast<volatile std::uint16_t*>(at) = static_cast<std::uint16_t>(v);
    } else {
      *reinterpret_cast<volatile std::uint8_t*>(at) = static_cast<std::uint8_t>(v);
    }
  }
}

struct HitRun {
  HitCounts alias;   // read through the alias right after the stores
  HitCounts memory;  // read through 0x7000_0000
  HitCounts inval;   // read through the alias after a full invalidate
};

// Do the write-hit step on the block at `alias_base` / `direct_base` (the same bytes), cache on. Leaves the block with the stores in it: the caller puts the scratch back.
HitRun hit_step(std::uintptr_t alias_base, std::uintptr_t direct_base) noexcept {
  HitRun r;
  r.alias.lines = r.memory.lines = r.inval.lines = kHitLines;
  const volatile std::uint32_t* const a = reinterpret_cast<const volatile std::uint32_t*>(alias_base);
  volatile std::uint32_t* const d = reinterpret_cast<volatile std::uint32_t*>(direct_base);
  std::uint32_t got[4];
  // 1. The base pattern, through 0x7000_0000, and then nothing is held (a full invalidate).
  for (std::uint32_t line = 0; line < kHitLines; ++line) {
    for (unsigned w = 0; w < 4; ++w) {
      d[line * 4u + w] = hit_word(line, w, HitImage::Base);
    }
  }
  asm volatile("dsb" ::: "memory");
  (void)cache::invalidate_all();
  // 2. Read the block through the alias: the cache holds the lines now (and the base pattern is what memory has).
  for (std::uint32_t line = 0; line < kHitLines; ++line) {
    read_line(a, line, got);
    for (unsigned w = 0; w < 4; ++w) {
      r.alias.base_bad += got[w] != hit_word(line, w, HitImage::Base) ? 1u : 0u;
    }
  }
  // 3. Behind the cache: the last byte of every line, through 0x7000_0000 (the probe).
  for (std::uint32_t line = 0; line < kHitLines; ++line) {
    *reinterpret_cast<volatile std::uint8_t*>(direct_base + line * lmem::kLineBytes + kHitProbe) = hit_probe_byte(line);
  }
  asm volatile("dsb" ::: "memory");
  // 4. The stores, through the alias: 32, 16 and 8 bits, inside lines that are held.
  for (std::uint32_t line = 0; line < kHitLines; ++line) {
    store_line(alias_base + line * lmem::kLineBytes, line);
  }
  asm volatile("dsb" ::: "memory");  // (and the write buffer has the whole readback below to drain before anything reads the other address)
  // 5. Read back through the alias: a write hit must have updated the cached copy.
  for (std::uint32_t line = 0; line < kHitLines; ++line) {
    read_line(a, line, got);
    const HitJudge j = hit_judge_alias(line, got);
    r.alias.held += j.held ? 1u : 0u;
    hit_count(r.alias, j.bad);
  }
  // 6. Through 0x7000_0000: write-through, so memory has the stores.
  for (std::uint32_t line = 0; line < kHitLines; ++line) {
    read_line(d, line, got);
    hit_count(r.memory, hit_judge_memory(line, got));
  }
  // 7. Through the alias after a full invalidate: the lines refill from memory.
  (void)cache::invalidate_all();
  for (std::uint32_t line = 0; line < kHitLines; ++line) {
    read_line(a, line, got);
    hit_count(r.inval, hit_judge_memory(line, got));
  }
  return r;
}

}  // namespace

void check_coherence(Emit emit, void* ctx) noexcept {
  const Out out{emit, ctx, Variant::Normal};
  char buf[256];
  if (!alias_enabled()) {
    out.line(buf, format_check_no_alias(buf, sizeof buf));
    return;
  }
  const std::uintptr_t direct = mem::kUiCacheBase + kCheckOffset;
  const std::uintptr_t alias = mem::sdram_alias(mem::kUiCacheBase) + kCheckOffset;
  volatile std::uint32_t* const a = reinterpret_cast<volatile std::uint32_t*>(alias);
  volatile std::uint32_t* const d = reinterpret_cast<volatile std::uint32_t*>(direct);
  const bool cache_on = cache::enabled();
  Tally tally;

  // 1. Written and read through the alias, read through the system bus.
  Noise n;
  for (std::uint32_t i = 0; i < kCheckWords; ++i) {
    a[i] = static_cast<std::uint32_t>(n.next()) * 65537u + i;
  }
  std::uint32_t bad_alias = 0;
  std::uint32_t bad_direct = 0;
  Noise m;
  for (std::uint32_t i = 0; i < kCheckWords; ++i) {
    const std::uint32_t want = static_cast<std::uint32_t>(m.next()) * 65537u + i;
    bad_alias += a[i] != want ? 1u : 0u;
    bad_direct += d[i] != want ? 1u : 0u;
  }
  Verdict v = judge_scratch(bad_alias, bad_direct);
  tally.add(Part::Scratch, v);
  out.line(buf, format_check_scratch(v, kCheckWords, bad_alias, bad_direct, buf, sizeof buf));

  // 2. The negative control: read the first words through the alias (their lines are cached), change them through the OTHER address, read through the alias again.
  std::uint32_t before = 0;
  for (std::uint32_t i = 0; i < kControlWords; ++i) {
    before += a[i];
  }
  asm volatile("" : : "r"(before));
  for (std::uint32_t i = 0; i < kControlWords; ++i) {
    d[i] = ~d[i];
  }
  asm volatile("dsb" ::: "memory");
  std::uint32_t stale = 0;
  Noise k;
  for (std::uint32_t i = 0; i < kControlWords; ++i) {
    const std::uint32_t old_value = static_cast<std::uint32_t>(k.next()) * 65537u + i;
    stale += a[i] == old_value ? 1u : 0u;  // the alias still says what it said: the line is held
  }
  (void)cache::invalidate_range(static_cast<std::uint32_t>(alias), kControlWords * 4u);
  std::uint32_t stale_after = 0;
  Noise k2;
  for (std::uint32_t i = 0; i < kControlWords; ++i) {
    const std::uint32_t old_value = static_cast<std::uint32_t>(k2.next()) * 65537u + i;
    stale_after += a[i] == old_value ? 1u : 0u;
  }
  v = judge_control(cache_on, kControlWords, stale, stale_after);  // (more than half, not all: the code this loop runs from flash takes some of the cache's sets, and a line evicted reads fresh)
  tally.add(Part::Control, v);
  out.line(buf, format_check_control(v, kControlWords, stale, stale_after, buf, sizeof buf));
  for (std::uint32_t i = 0; i < kControlWords; ++i) {
    a[i] = ~d[i];  // put the scratch back (through the alias, which is the rule)
  }

  // 3. Write hits: stores of 32, 16 and 8 bits into lines the cache holds, read back through the alias, through 0x7000_0000 and through the alias after a full invalidate (hit_step).
  if (cache_on) {
    const HitRun hit = hit_step(alias + kHitFirstWord * 4u, direct + kHitFirstWord * 4u);
    v = judge_hit_alias(true, hit.alias);
    tally.add(Part::HitAlias, v);
    out.line(buf, format_check_hit_alias(v, hit.alias, buf, sizeof buf));
    v = judge_hit_clean(true, hit.memory);
    tally.add(Part::HitMemory, v);
    out.line(buf, format_check_hit_memory(v, hit.memory, buf, sizeof buf));
    v = judge_hit_clean(true, hit.inval);
    tally.add(Part::HitInval, v);
    out.line(buf, format_check_hit_inval(v, hit.inval, buf, sizeof buf));
    // 4. Put the block back to what step 1 wrote (the same noise), through the alias, and read it through both addresses.
    Noise skip;
    for (std::uint32_t i = 0; i < kHitFirstWord; ++i) {
      (void)skip.next();
    }
    Noise put = skip;
    for (std::uint32_t i = kHitFirstWord; i < kHitFirstWord + kHitWords; ++i) {
      a[i] = static_cast<std::uint32_t>(put.next()) * 65537u + i;
    }
    asm volatile("dsb" ::: "memory");
    std::uint32_t restore_alias = 0;
    std::uint32_t restore_direct = 0;
    for (std::uint32_t i = kHitFirstWord; i < kHitFirstWord + kHitWords; ++i) {
      const std::uint32_t want = static_cast<std::uint32_t>(skip.next()) * 65537u + i;
      restore_alias += a[i] != want ? 1u : 0u;
      restore_direct += d[i] != want ? 1u : 0u;
    }
    v = judge_restore(restore_alias, restore_direct);
    tally.add(Part::Restore, v);
    out.line(buf, format_check_restore(v, kHitWords, restore_alias, restore_direct, buf, sizeof buf));
  } else {
    tally.add(Part::HitAlias, Verdict::Skip);
    out.line(buf, format_check_hit_skip(buf, sizeof buf));
  }

  // 5. The loaded samples: the same bytes through both addresses and again through the alias after a full invalidate.
  const std::uint32_t bytes = appimg::sample_pcm_bytes() & ~3u;
  const std::uint32_t words = bytes / 4u;
  const std::uint32_t s_alias = sum_words(mem::sdram_alias(mem::kSampleArenaBase), words);
  const std::uint32_t s_direct = sum_words(mem::kSampleArenaBase, words);
  (void)cache::invalidate_all();
  const std::uint32_t s_inv = sum_words(mem::sdram_alias(mem::kSampleArenaBase), words);
  v = judge_pcm(s_alias, s_direct, s_inv);
  tally.add(Part::Pcm, v);
  out.line(buf, format_check_pcm(v, bytes, s_alias, s_direct, s_inv, buf, sizeof buf));
  out.line(buf, format_check_final(tally, buf, sizeof buf));
}

// ---- `bench disk` (OS-USBDISK U1d; bench_disk.h has the lines and what each number is for) --------------------------------------------------------------
// The card READS (single block as the MSC engine does it, then 8 and 32 blocks as CMD18), the WRITE arm, the window of the USB interrupt and the window of the pump's passes. The reads are raw blocks from
// the start of the card, with the OS's own mount untouched (a read is not a FatFs call; the main loop is the only SD user). They run only while the card is the Tracker's, started and idle: under a computer
// (owner Host) the pump owns the bus, and the window lines are what to read then. The render is not held off: the figures are the wall time a copy would see, the max carries the interrupts.
//
// THE WRITE ARM (Q14, maintainer 2026-10-17; save/bench_scratch.h has the gate and the file). F6 (iii) stands: the bench never writes a raw LBA. It writes ONLY through FatFs, ONLY into the pre-allocated
// scratch file `o3-bench/disk-scratch.bin` (made and filled once, kept; `bench disk clean` removes it), and ONLY on a card that the Tracker owns (any SD card: OS-ANYCARD); anything else is
// one `BENCH disk_wr n/a: <why>` line and not a byte written. Each arm rewrites blocks inside the file, so a timed call allocates no cluster. One call = one f_write of 1, 8 or 32 blocks.
namespace {
namespace diskbench {
struct Base {
  bool open = false;
  std::uint32_t ms = 0;
  std::uint32_t entries = 0;
  std::uint32_t cycles = 0;
  std::uint32_t in_packets = 0;
  std::uint32_t out_packets = 0;
};
Base g_base;

std::uint32_t now_ms() noexcept { return static_cast<std::uint32_t>(system_clock().uptime_ms()); }

// The reason the read arms cannot run, or null.
const char* read_blocker(const storage::BlockDevice& dev) noexcept {
  if (storage::device::owner() != storage::CardOwner::Tracker) {
    return "the card is not the Tracker's (a computer has it, or no card)";
  }
  if (storage::device::card_busy()) {
    return "the card is busy (a save, load or export holds it)";
  }
  if (!dev.ready()) {
    return "the card is not started";
  }
  return nullptr;
}

void read_arm(const Out& out, storage::BlockDevice& dev, const char* name, std::uint32_t per_call, std::uint32_t calls, std::uint32_t base) noexcept {
  char buf[200];
  const std::uint32_t total = dev.block_count();
  const std::uint32_t fit = total / per_call;
  const std::uint32_t n = fit < calls ? fit : calls;
  if (n < 2u || base + (n + 1u) * per_call > total) {
    out.line(buf, format_na(name, "the card is too small for the run", buf, sizeof buf));
    return;
  }
  std::uint8_t* const dst = reinterpret_cast<std::uint8_t*>(mem::kUiCacheBase);  // 16 KiB of the unused ui_cache region (bench scratch), the core's direct address (the SDHC is polled)
  static_assert(32u * 512u <= mem::kUiCacheSize, "32 blocks fit the scratch");
  std::uint32_t lba = base;
  if (dev.read(lba, per_call, dst) != storage::BlockResult::Ok) {  // untimed: the card wakes, the first command of the arm
    out.line(buf, format_na(name, "the first read failed", buf, sizeof buf));
    return;
  }
  Stat st;
  for (std::uint32_t i = 0; i < n; ++i) {
    lba += per_call;
    const std::uint32_t t0 = dwt();
    const storage::BlockResult r = dev.read(lba, per_call, dst);
    const std::uint32_t dt = dwt() - t0;
    if (r != storage::BlockResult::Ok) {
      out.line(buf, format_na(name, "a read failed", buf, sizeof buf));
      return;
    }
    st.add(dt);
  }
  out.line(buf, disk::format_read(name, st, per_call, buf, sizeof buf));
}

// The write arm's memory, in the unused ui_cache region after the read arms' 16 KiB: 16 KiB of source (32 blocks of a pattern) and the scratch file object (a FIL), zero SRAM.
constexpr std::uint32_t kWriteSrcOffset = 16u * 1024u;
constexpr std::uint32_t kScratchOffset = 32u * 1024u;
static_assert(kWriteSrcOffset >= 32u * 512u, "the read arms' buffer is below the write source");
static_assert(kScratchOffset >= kWriteSrcOffset + 32u * 512u, "the write source is below the scratch object");
static_assert(kScratchOffset + sizeof(save::BenchScratch) <= mem::kUiCacheSize, "the scratch object fits the ui_cache region");
static_assert((mem::kUiCacheBase + kWriteSrcOffset) % 4u == 0u && (mem::kUiCacheBase + kScratchOffset) % alignof(save::BenchScratch) == 0u, "word-aligned buffers (the SDHC FIFO moves words)");

void write_arm(const Out& out, save::BenchScratch& sc, const char* name, std::uint32_t per_call, std::uint32_t calls, const std::uint8_t* src) noexcept {
  char buf[200];
  const std::uint32_t fit = sc.blocks() / per_call;  // calls the file holds: one is the warm-up, the rest are timed, all inside the file
  const std::uint32_t n = (fit > 1u ? fit - 1u : 0u) < calls ? (fit > 1u ? fit - 1u : 0u) : calls;
  if (n < 2u) {
    out.line(buf, format_na(name, "the scratch file is too small for the run", buf, sizeof buf));
    return;
  }
  save::BenchResult r = sc.rewrite(0, src, per_call);  // untimed: the card wakes for a write, the seek back to the start of the file
  disk::CallStat st;
  for (std::uint32_t i = 0; i < n && r == save::BenchResult::Ok; ++i) {
    const std::uint32_t t0 = dwt();
    r = sc.rewrite((i + 1u) * per_call, src, per_call);
    const std::uint32_t dt = dwt() - t0;
    st.add(dt);
  }
  if (r != save::BenchResult::Ok) {
    out.line(buf, format_na(name, save::bench_result_name(r), buf, sizeof buf));
    return;
  }
  out.line(buf, disk::format_write(name, st, per_call, buf, sizeof buf));
}

// prepare()'s notice before the one-time fill (Out is the context).
void say_filling(void* ctx) noexcept {
  char buf[200];
  static_cast<const Out*>(ctx)->line(buf, disk::format_filling(save::kBenchScratchBytes, buf, sizeof buf));
}

void write_run(const Out& out, std::uint32_t calls) noexcept {
  char buf[260];
  std::uint8_t* const src = reinterpret_cast<std::uint8_t*>(mem::kUiCacheBase + kWriteSrcOffset);
  for (std::uint32_t i = 0; i < 32u * 512u; ++i) {
    src[i] = static_cast<std::uint8_t>(i * 131u + (i >> 9) * 17u + 1u);  // a pattern that is not all zeros or all ones (nothing the card could shortcut)
  }
  save::BenchScratch* const sc = ::new (reinterpret_cast<void*>(mem::kUiCacheBase + kScratchOffset)) save::BenchScratch(storage::device::fat(), storage::device::block());
  const std::uint32_t t0 = now_ms();
  const save::BenchResult p = sc->prepare(storage::device::owner(), storage::device::card_busy(), src, 32u * 512u, say_filling, const_cast<Out*>(&out));
  if (p != save::BenchResult::Ok) {
    out.line(buf, format_na("disk_wr", save::bench_result_name(p), buf, sizeof buf));
    return;
  }
  out.line(buf, disk::format_scratch(save::kBenchScratchPath, sc->blocks() / 2u, sc->filled_bytes(), now_ms() - t0, buf, sizeof buf));
  static constexpr struct {
    const char* name;
    std::uint32_t per_call;
  } kArms[] = {{"disk_wr1", 1u}, {"disk_wr8", 8u}, {"disk_wr32", 32u}};
  for (const auto& arm : kArms) {
    write_arm(out, *sc, arm.name, arm.per_call, calls, src);
  }
  const save::BenchResult f = sc->finish();  // the directory entry (the modification time) goes to the card here, untimed
  if (f != save::BenchResult::Ok) {
    out.line(buf, format_na("disk_wr", save::bench_result_name(f), buf, sizeof buf));
  }
  out.line(buf, disk::format_write_driver(buf, sizeof buf));
}

void run(const Out& out, std::uint32_t calls) noexcept {
  char buf[260];
  storage::BlockDevice& dev = storage::device::block();
  const char* why = read_blocker(dev);
  static constexpr struct {
    const char* name;
    std::uint32_t per_call;
  } kArms[] = {{"disk_rd1", 1u}, {"disk_rd8", 8u}, {"disk_rd32", 32u}};
  std::uint32_t arm_index = 0;
  for (const auto& arm : kArms) {
    if (why != nullptr) {
      out.line(buf, format_na(arm.name, why, buf, sizeof buf));
    } else {
      read_arm(out, dev, arm.name, arm.per_call, calls, arm_index * 32768u);  // each arm from another stretch of the card
    }
    ++arm_index;
  }
  write_run(out, calls);
  disk::UsbWindow w;
  if (g_base.open) {
    const o3os::usb::DeviceCounters& c = usb0::device_counters();
    w.ms = now_ms() - g_base.ms;
    w.entries = c.isr_count - g_base.entries;
    w.cycles = c.isr_cycles_sum - g_base.cycles;
    w.in_packets = c.msc_in_packets - g_base.in_packets;
    w.out_packets = c.msc_out_packets - g_base.out_packets;
    w.last = c.isr_cycles_last;
    w.max = c.isr_cycles_max;
  }
  out.line(buf, disk::format_usb(w, buf, sizeof buf));
  out.line(buf, disk::format_pass(w.ms, usb0::disk_pass_stat(), buf, sizeof buf));
  out.line(buf, disk::format_bounds(buf, sizeof buf));
}
}  // namespace diskbench
}  // namespace

void disk_reset() noexcept {
  const o3os::usb::DeviceCounters& c = usb0::device_counters();
  diskbench::g_base.open = true;
  diskbench::g_base.ms = diskbench::now_ms();
  diskbench::g_base.entries = c.isr_count;
  diskbench::g_base.cycles = c.isr_cycles_sum;
  diskbench::g_base.in_packets = c.msc_in_packets;
  diskbench::g_base.out_packets = c.msc_out_packets;
  usb0::bench_window_reset();
}

void disk_clean(Emit emit, void* ctx) noexcept {
  const Out out{emit, ctx, Variant::Normal};
  char buf[200];
  save::BenchScratch* const sc = ::new (reinterpret_cast<void*>(mem::kUiCacheBase + diskbench::kScratchOffset)) save::BenchScratch(storage::device::fat(), storage::device::block());
  const save::BenchResult r = sc->clean(storage::device::owner(), storage::device::card_busy());
  out.line(buf, disk::format_clean(save::kBenchScratchPath, r == save::BenchResult::Ok ? nullptr : save::bench_result_name(r), buf, sizeof buf));
}

void run(Kernel kernel, std::uint32_t blocks, Variant variant, Emit emit, void* ctx) noexcept {
  const Out out{emit, ctx, variant == Variant::Count ? Variant::Normal : variant};
  blocks = blocks == 0u ? kDefaultBlocks : (blocks > kMaxBlocks ? kMaxBlocks : blocks);
  switch (kernel) {
    case Kernel::Voice: run_voice(out, blocks, VoiceMode::Plain); break;
    case Kernel::VoiceFilter: run_voice(out, blocks, VoiceMode::Filter); break;
    case Kernel::VoiceDrive: run_voice(out, blocks, VoiceMode::Drive); break;
    case Kernel::VoiceCrush: run_voice(out, blocks, VoiceMode::Crush); break;
    case Kernel::VoiceBoth: run_voice(out, blocks, VoiceMode::Both); break;
    case Kernel::VoiceRev: run_voice(out, blocks, VoiceMode::Rev); break;
    case Kernel::VoicePan: run_voice(out, blocks, VoiceMode::Pan); break;
    case Kernel::VoiceSt: voice_st::run(out, blocks); break;
    case Kernel::Tb303: run_tb303(out, blocks, kBlock); break;
    case Kernel::Tb303x128: run_tb303(out, blocks, 2u * kBlock); break;
    case Kernel::Delay: run_delay(out, blocks); break;
    case Kernel::Reverb: run_reverb(out, blocks, false); break;
    case Kernel::ReverbFull: run_reverb(out, blocks, true); break;
    case Kernel::Limiter: run_limiter(out, blocks, true); break;
    case Kernel::LimiterQuiet: run_limiter(out, blocks, false); break;
    case Kernel::Strips: run_stages(out, blocks, true); break;
    case Kernel::Stages: run_stages(out, blocks, false); break;
    case Kernel::Mem: run_mem(out); break;
    case Kernel::Worst: run_worst(out); break;
    case Kernel::Kernels: run_kernels(out, blocks); break;
    case Kernel::Cpuopt: cpuopt::run(out, blocks); break;
    case Kernel::Looper: looper::run(out, blocks); break;
    case Kernel::Disk: diskbench::run(out, blocks); break;
    case Kernel::Burst: run_burst(out, blocks); break;
    case Kernel::DjMaster: run_dj(out, blocks); break;
    case Kernel::Count: break;
  }
}

}  // namespace o3os::platform::mk66::bench
