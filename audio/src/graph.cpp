// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/audio/graph.h>

#include <o3os/audio/kernels_impl.h>
#include <o3os/core/fast_math.h>
#include <o3os/usb/kernels.h>

namespace o3os::audio {
namespace {

constexpr std::uint32_t kBlk = Graph::kBlock;
constexpr std::uint32_t kAllStrips = (1u << Graph::kStrips) - 1u;

constexpr std::uint8_t kDestBit[3] = {route::kMain, route::kCue, route::kRec};

O3OS_AUDIO_INLINE bool in_range(float v, float lo, float hi) noexcept { return v >= lo && v <= hi; }  // false for NaN
O3OS_AUDIO_INLINE std::uint32_t as_uint(float v) noexcept { return static_cast<std::uint32_t>(v + 0.5f); }

// OS-PERFFX P3: a send's level on the fader's 0..100: the strip's own send plus the instrument's (or the step FX's) held to 100, plus the Perform layer's offset, held to 0..100 again.
O3OS_AUDIO_INLINE float send_level(float sum, std::int8_t offset) noexcept {
  const float v = (sum > 100.0f ? 100.0f : sum) + static_cast<float>(offset);
  return v < 0.0f ? 0.0f : (v > 100.0f ? 100.0f : v);
}

// A block of kBlk samples, a word at a time (both 4-byte aligned): the feed into a scratch block, a track's block into the stem staging.
O3OS_AUDIO_INLINE void copy_block(std::int16_t* dst, const std::int16_t* src) noexcept {
  for (std::uint32_t i = 0; i < kBlk; i += 2u) {
    kernel::detail::store_pair(dst + i, kernel::detail::load_pair(src + i));
  }
}

}  // namespace

// ---- The level laws (boot / load) --------------------------------------------------------------------------------------------

std::int32_t level_gain_q16(unsigned level, LevelLaw law) noexcept {
  // Og1114: 10^((i - 100) / 50) = 2^((i - 100) * log2(10) / 50). Og12 (smoothing.h, THE LEVEL LAWS): -24 dB at level 1, then 0.48 dB x (level - 50), as
  // gain 2^(dB * log2(10) / 20). Level 0 of both is a true zero, Og1114's 100 and Og12's 50 are unity.
  constexpr float kStep1114 = 0.0664385619f;  // log2(10) / 50: a level step of 0.4 dB
  constexpr float kStep12 = 0.0797262743f;    // 0.48 x log2(10) / 20: a level step of 0.48 dB
  constexpr float kLevel1 = -3.9863137f;      // -24 x log2(10) / 20: Og12's level 1
  if (level == 0u) {
    return 0;
  }
  if (level > kLevelSteps) {
    level = kLevelSteps;
  }
  float log2_gain;
  if (law == LevelLaw::Og1114) {
    if (level == kLevelSteps) {
      return kernel::kUnity;
    }
    log2_gain = static_cast<float>(static_cast<int>(level) - static_cast<int>(kLevelSteps)) * kStep1114;
  } else {
    if (level == 50u) {
      return kernel::kUnity;
    }
    log2_gain = level == 1u ? kLevel1 : static_cast<float>(static_cast<int>(level) - 50) * kStep12;
  }
  return static_cast<std::int32_t>(core::exp2_fast(log2_gain) * 65536.0f + 0.5f);
}

void LevelCurve::init() noexcept {
  for (unsigned i = 0; i <= kLevelSteps; ++i) {
    table_[i] = level_gain_q16(i, LevelLaw::Og1114);
  }
}

// ---- Construction and configuration -----------------------------------------------------------------------------------------

Graph::Graph(CycleFn cycles, TapMem& mem) noexcept
    : cycles_(cycles), mem_((mem.reset(), mem)), meters_(mem.meters), stems_(mem.stem) {
  init_defaults();
}

void Graph::init_defaults() noexcept {
  curve_.init();
  for (unsigned i = 0; i < kStrips; ++i) {
    Strip& s = strips_[i];
    s.active = i >= kInputStrip || ((kDefaultActiveMask >> i) & 1u) != 0u;
    s.tone.configure(tone_phase_inc_q32(test_source_hz(i < kTracks ? i : 0u)), kToneAmplitude, kToneRampSamples);
    s.noise = 0x2545F491u + 0x9E3779B9u * (i + 1u);  // distinct, never 0
    s.source = i < 2u ? SourceKind::Sine : SourceKind::None;  // the HW-A script's two tones; off until the script or a ParamChange starts them
    if (i < kTracks) {
      // ADR 0012 Decision 3 (OS-006d): a track is in the USB column from the start, so that the default stem mapping (T1..T8 to S1..S8) works with no routing
      // edit. The bit costs nothing until a host streams: a track's block is staged for the tap only while the stem tap is enabled.
      s.route = static_cast<std::uint8_t>(route::kMain | route::kUsb);
    }
    if (i == kInputStrip) {
      s.volume = 0.0f;  // the monitor is silent until the app raises it (see the header)
      s.route = static_cast<std::uint8_t>(route::kMain | route::kCue);
    }
  }
  recompute_master(1);
  for (unsigned i = 0; i < kStrips; ++i) {
    recompute_strip(i, 1);
    strips_[i].settled = true;
  }
  install_master_stage(MasterStage::Bass, &MasterDjFilter::process_fx, &dj_filter_);  // OS-DJ A2: installed, bypassed until the DJ page gives it a position
  dj_filter_.attach(&master_[static_cast<unsigned>(MasterStage::Bass)].bypass);
  install_master_stage(MasterStage::Eq, &MasterKillEq::process_fx, &kill_eq_);  // OS-DJ A3: the same, for the kill EQ
  kill_eq_.attach(&master_[static_cast<unsigned>(MasterStage::Eq)].bypass);
  install_master_stage(MasterStage::Headroom, &MasterScratch::process_fx, &scratch_stage_);  // OS-DJ C1: the scratch ring is the first stage, so the kills, the filter and the limiter act on the scratched sound
  scratch_stage_.attach(&master_[static_cast<unsigned>(MasterStage::Headroom)].bypass);
}

void Graph::kill_engage() noexcept {
  StageSlot& slot = master_[static_cast<unsigned>(MasterStage::Eq)];
  if (slot.fn == &MasterKillEq::process_fx && !kill_eq_.idle()) {
    slot.bypass = false;
  }
}

void Graph::dj_engage() noexcept {
  StageSlot& slot = master_[static_cast<unsigned>(MasterStage::Bass)];
  if (slot.fn == &MasterDjFilter::process_fx && !dj_filter_.idle()) {
    slot.bypass = false;
  }
}

void Graph::configure(std::uint32_t active_mask) noexcept {
  any_solo_ = false;
  for (unsigned t = 0; t < kTracks; ++t) {
    Strip& s = strips_[t];
    s.active = ((active_mask >> t) & 1u) != 0u;
    any_solo_ = any_solo_ || (s.active && s.solo);
  }
  dirty_ = 0;
  pan_only_ = 0;
  for (unsigned i = 0; i < kStrips; ++i) {
    recompute_strip(i, 1);
  }
}

void Graph::install_insert(unsigned track, unsigned slot, InsertFn fn, void* state) noexcept {
  if (track < kTracks && slot < 2u) {
    strips_[track].insert[slot] = Insert{fn, state, true};
  }
}
void Graph::set_insert_bypass(unsigned track, unsigned slot, bool bypass) noexcept {
  if (track < kTracks && slot < 2u) {
    strips_[track].insert[slot].bypass = bypass;
    if (bypass) {
      strips_[track].insert[slot].tail = 0;  // a bypassed slot has no tail (OS-INSFX S2)
    }
  }
}
void Graph::install_send_fx(unsigned which, SendFxFn fn, void* state, SendFxParamFn param) noexcept {
  if (which < 2u) {
    fx_[which] = FxSlot{fn, state, true, param};
  }
}
void Graph::set_send_fx_tempo(std::uint32_t tempo_x100) noexcept {
  for (unsigned a = 0; a < 2u; ++a) {
    if (fx_[a].param != nullptr) {
      (void)fx_[a].param(fx_[a].state, sendfx::kTempoField, static_cast<float>(tempo_x100));  // (an effect with no use for it refuses)
    }
  }
}
void Graph::set_sends_post_fader(bool post) noexcept {
  if (sends_post_ == post) {
    return;
  }
  sends_post_ = post;
  for (unsigned i = 0; i < kStrips; ++i) {
    ramp_[i] = ((dirty_ >> i) & 1u) != 0u ? ramp_[i] : 0u;  // the engine's own smoothing: a send does not jump when the setting is flipped (a strip with a change pending keeps the ramp that change asked for)
  }
  mark_all_dirty();
}
void Graph::reset_voice_sends() noexcept {
  for (unsigned t = 0; t < kTracks; ++t) {
    Strip& s = strips_[t];
    if (s.inst_send_a != 0u || s.inst_send_b != 0u) {
      s.inst_send_a = 0u;
      s.inst_send_b = 0u;
      ramp_[t] = ((dirty_ >> t) & 1u) != 0u ? ramp_[t] : 0u;
      mark_dirty(t);
    }
  }
  clear_step_sends(0u);  // OS-PERFFX P3: the step FX's held sends are the last song's too (the Perform layer's are not: they are the player's)
}
void Graph::set_voice_sends(unsigned track, unsigned delay, unsigned reverb, std::uint32_t ramp) noexcept {
  if (track >= kTracks) {
    return;
  }
  Strip& s = strips_[track];
  const std::uint8_t a = static_cast<std::uint8_t>(delay > 100u ? 100u : delay);
  const std::uint8_t b = static_cast<std::uint8_t>(reverb > 100u ? 100u : reverb);
  if (s.inst_send_a != a || s.inst_send_b != b) {
    s.inst_send_a = a;
    s.inst_send_b = b;
    ramp_[track] = ((dirty_ >> track) & 1u) != 0u ? ramp_[track] : ramp;  // a note starts now (instant), a live edit smooths; a strip the drain already has a request for keeps that request's ramp
    mark_dirty(track);
  }
}
void Graph::set_send_fx_bypass(unsigned which, bool bypass) noexcept {
  if (which < 2u) {
    fx_[which].bypass = bypass;
  }
}
void Graph::install_master_stage(MasterStage id, MasterFn fn, void* state) noexcept {
  if (static_cast<unsigned>(id) < kMasterStages) {
    master_[static_cast<unsigned>(id)] = StageSlot{fn, state, true};
  }
}
void Graph::set_master_bypass(MasterStage id, bool bypass) noexcept {
  if (static_cast<unsigned>(id) < kMasterStages) {
    master_[static_cast<unsigned>(id)].bypass = bypass;
  }
}
void Graph::install_master_limiter(MasterLimiter& limiter) noexcept {
  limiter.reset();
  limiter_ = &limiter;
  install_master_stage(MasterStage::Limiter, &MasterLimiter::process_fx, &limiter);
  set_master_bypass(MasterStage::Limiter, false);  // the base limiter is always on: the master LIMITER switch is the limiter's own (its controls in force), not a bypass
}

void Graph::activate(unsigned track) noexcept {
  if (track < kTracks) {
    strips_[track].active = true;
    if (strips_[track].solo && !any_solo_) {
      any_solo_ = true;
      mark_all_dirty();  // a solo changes what every other track is (and the B1 pan-only path assumes every level and send input marks its strip)
    }
  }
}

void Graph::use_feed_sources(std::uint32_t mask) noexcept {
  for (unsigned t = 0; t < kTracks; ++t) {
    if (((mask >> t) & 1u) != 0u) {
      strips_[t].source = SourceKind::Feed;
      strips_[t].tone.set_on(false);
    }
  }
}

void Graph::script_tone(unsigned track, bool on) noexcept {
  if (track < kTracks) {
    strips_[track].source = SourceKind::Sine;
    strips_[track].tone.set_on(on);
  }
}

// ---- Parameters --------------------------------------------------------------------------------------------------------------

void Graph::drain() noexcept {
  ParamChange c;
  std::uint32_t drained = 0;
  while (drained < kMaxParamsPerBlock && params_.pop(c)) {
    apply(c);
    ++drained;
  }
  if (drained != 0u) {
    applied_.store(applied_.load(std::memory_order_relaxed) + drained, std::memory_order_relaxed);
  }
  std::uint32_t d = dirty_;
  std::uint32_t pan_only = pan_only_;
  dirty_ = 0;
  pan_only_ = 0;
  for (unsigned i = 0; d != 0u && i < kStrips; ++i, d >>= 1, pan_only >>= 1) {
    if ((d & 1u) != 0u) {
      // OS-CPU10 B1: a strip whose only change is its pan (the panbrello, a pan FX) retargets its pan gains alone when its level and send gains are at rest: their retarget would be a no-op.
      const Strip& s = strips_[i];
      if ((pan_only & 1u) != 0u && !s.level.ramping() && !s.send_gain[0].ramping() && !s.send_gain[1].ramping()) {
        recompute_pan(i, ramp_[i]);
      } else {
        recompute_strip(i, ramp_[i]);
      }
    }
  }
}

void Graph::apply(const ParamChange& c) noexcept {
  bool ok = false;
  const unsigned index = core::param_index(c.id);
  const unsigned field = core::param_field(c.id);
  if (c.value == c.value) {  // not NaN
    switch (core::param_kind(c.id)) {
      case core::ParamKind::Track:
        ok = index < kStrips && apply_strip(index, field, c.value, c.ramp_samples);
        break;
      case core::ParamKind::Master:
        ok = index == 0u && apply_master(field, c.value, c.ramp_samples);
        break;
      case core::ParamKind::Project:
        if (index == 0u && field == graph_param::project::SendsMode) {  // OS-SENDFX: the project's SENDS setting
          ok = c.value == 0.0f || c.value == 1.0f;
          if (ok) {
            set_sends_post_fader(c.value != 0.0f);
          }
          break;
        }
        if (index == 0u && field == graph_param::project::LimiterMode && limiter_ != nullptr) {  // OS-LIMIT: the project's LIMITER MODE, Subtle or Extreme
          ok = apply_limiter_mode(c.value);
          break;
        }
        ok = param_hook_ != nullptr && param_hook_(param_self_, c);
        break;
      case core::ParamKind::Instrument:
        if (inst_hook_ != nullptr) {
          ok = inst_hook_(inst_self_, c);  // the sampler voices' descriptors (OS-LIVEP)
          break;
        }
        [[fallthrough]];
      default:
        ok = param_hook_ != nullptr && param_hook_(param_self_, c);  // the sequencer's (OS-005d): a project-kind engine field
        break;
    }
  }
  if (!ok) {
    rejected_.store(rejected_.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
  }
}

void Graph::set_voice_pan(unsigned track, unsigned instrument_pan, std::uint32_t ramp) noexcept {
  if (track >= kTracks) {
    return;
  }
  const float offset = static_cast<float>(instrument_pan > 100u ? 100u : instrument_pan) - 50.0f;
  Strip& s = strips_[track];
  if (s.pan_offset != offset) {
    s.pan_offset = offset;
    ramp_[track] = ((dirty_ >> track) & 1u) != 0u && ramp_[track] == 1u ? 1u : ramp;  // a note's start (ramp 1: at once) in this very block is kept by the autopan's smoothed update that follows it (OS-MOD)
    mark_pan_dirty(track);  // recomputed by the drain of this block's graph render, which follows the voices
  }
}

void Graph::set_track_pan(unsigned track, float pan, std::uint32_t ramp) noexcept {
  if (track >= kTracks) {
    return;
  }
  Strip& s = strips_[track];
  const float offset = (pan < 0.0f ? 0.0f : (pan > 100.0f ? 100.0f : pan)) - s.pan;  // (a NaN is 0)
  if (s.pan_offset != offset) {
    s.pan_offset = offset;
    ramp_[track] = ((dirty_ >> track) & 1u) != 0u && ramp_[track] == 1u ? 1u : ramp;  // (set_voice_pan's rule: a note's instant pan in this block is kept)
    mark_pan_dirty(track);
  }
}

void Graph::set_track_gain(unsigned track, float gain, std::uint32_t ramp) noexcept {
  if (track >= kTracks) {
    return;
  }
  Strip& s = strips_[track];
  const float g = gain >= 0.0f ? (gain > 1.0f ? 1.0f : gain) : 0.0f;  // (a NaN is 0)
  if (s.trk_gain != g) {
    s.trk_gain = g;
    ramp_[track] = ((dirty_ >> track) & 1u) != 0u && ramp_[track] == 1u ? 1u : ramp;
    mark_dirty(track);
  }
}

// OS-PERFFX P0, P2 (graph.h): the Perform layer's values of the strips. A changed value marks the strip dirty (recompute_strip composes it); the ramp rule is the tracker setters'.
void Graph::set_perf(unsigned track, std::int8_t Strip::*field, int lo, int value, std::uint32_t ramp) noexcept {
  if (track >= kTracks) {
    return;
  }
  std::int8_t& f = strips_[track].*field;
  const std::int8_t v = static_cast<std::int8_t>(value < lo ? lo : (value > 100 ? 100 : value));
  if (f != v) {
    f = v;
    ramp_[track] = ((dirty_ >> track) & 1u) != 0u && ramp_[track] == 1u ? 1u : ramp;
    mark_dirty(track);
  }
}

void Graph::set_perf_gain(unsigned track, int value, std::uint32_t ramp) noexcept {
  set_perf(track, &Strip::perf_gain, -100, value, ramp);
}

void Graph::set_perf_pan(unsigned track, int value, std::uint32_t ramp) noexcept {
  set_perf(track, &Strip::perf_pan, -100, value, ramp);
}

// OS-PERFFX P3 (graph.h): the sends' two values of a strip; `which` other than 0 and 1 is ignored.
void Graph::set_perf_send(unsigned track, unsigned which, int value, std::uint32_t ramp) noexcept {
  if (which < 2u) {
    set_perf(track, which == 0u ? &Strip::perf_send_a : &Strip::perf_send_b, -100, value, ramp);
  }
}

void Graph::set_step_send(unsigned track, unsigned which, int value, std::uint32_t ramp) noexcept {
  if (which < 2u) {
    set_perf(track, which == 0u ? &Strip::step_send_a : &Strip::step_send_b, -1, value, ramp);  // (below 0: none)
  }
}

void Graph::clear_step_sends(std::uint32_t ramp) noexcept {
  for (unsigned t = 0; t < kTracks; ++t) {
    set_step_send(t, 0u, -1, ramp);
    set_step_send(t, 1u, -1, ramp);
  }
}

void Graph::perf_save(PerfSave& out) const noexcept {
  for (unsigned t = 0; t < kTracks; ++t) {
    out.gain[t] = strips_[t].perf_gain;
    out.pan[t] = strips_[t].perf_pan;
    out.send[0][t] = strips_[t].perf_send_a;
    out.send[1][t] = strips_[t].perf_send_b;
    out.xf[t] = strips_[t].xf_gain;
  }
  out.dj_filter = dj_filter_.target();
  out.kill_eq = static_cast<std::uint8_t>(kill_eq_.mask());
}

// The export's start and end: the strips that change are recomputed with ramp 1 (at once), so that the export's first block has no trace of the live layer's gain and the live render's first block has it
// whole, and not a smoothed climb through the export's levels.
void Graph::perf_clear() noexcept {
  scratch_stage_.abort();  // OS-DJ C1: the export carries no scratch (the app finds the stage idle and gives the ring back)
  for (unsigned t = 0; t < kTracks; ++t) {
    Strip& s = strips_[t];
    if (s.perf_gain != 0 || s.perf_pan != 0 || s.perf_send_a != 0 || s.perf_send_b != 0 || s.xf_gain != 255u) {
      s.xf_gain = 255;
      s.perf_gain = 0;
      s.perf_pan = 0;
      s.perf_send_a = 0;
      s.perf_send_b = 0;
      ramp_[t] = 1u;
      mark_dirty(t);
    }
  }
  if (!dj_filter_.idle()) {  // OS-DJ A2: the master DJ filter put away, state and all, and its stage off
    dj_filter_.jump(0.0f);
    master_[static_cast<unsigned>(MasterStage::Bass)].bypass = true;
  }
  if (!kill_eq_.idle()) {  // OS-DJ A3: the kill EQ put away the same way
    kill_eq_.reset(0u);
    master_[static_cast<unsigned>(MasterStage::Eq)].bypass = true;
  }
}

void Graph::perf_restore(const PerfSave& in) noexcept {
  for (unsigned t = 0; t < kTracks; ++t) {
    Strip& s = strips_[t];
    if (s.perf_gain != in.gain[t] || s.perf_pan != in.pan[t] || s.perf_send_a != in.send[0][t] || s.perf_send_b != in.send[1][t] || s.xf_gain != in.xf[t]) {
      s.xf_gain = in.xf[t];
      s.perf_gain = in.gain[t];
      s.perf_pan = in.pan[t];
      s.perf_send_a = in.send[0][t];
      s.perf_send_b = in.send[1][t];
      ramp_[t] = 1u;
      mark_dirty(t);
    }
  }
  if (in.dj_filter != dj_filter_.target()) {  // OS-DJ A2
    dj_filter_.jump(in.dj_filter);
    dj_engage();
  }
  if (in.kill_eq != kill_eq_.mask()) {  // OS-DJ A3: back with every gain at 1 and the kill ramped in (no step)
    kill_eq_.reset(in.kill_eq);
    kill_engage();
  }
}

void Graph::set_track_pan_mod(unsigned track, float offset, std::uint32_t ramp) noexcept {
  if (track >= kTracks) {
    return;
  }
  Strip& s = strips_[track];
  const float m = offset >= -100.0f ? (offset > 100.0f ? 100.0f : offset) : -100.0f;  // (a NaN is -100: the caller never sends one)
  if (s.pan_mod != m) {
    s.pan_mod = m;
    ramp_[track] = ((dirty_ >> track) & 1u) != 0u && ramp_[track] == 1u ? 1u : ramp;
    mark_pan_dirty(track);
  }
}

float Graph::track_pan(unsigned track) const noexcept {
  if (track >= kTracks) {
    return 50.0f;
  }
  const float p = strips_[track].pan + strips_[track].pan_offset;
  return p < 0.0f ? 0.0f : (p > 100.0f ? 100.0f : p);
}

void Graph::set_strip_mute(unsigned track, bool on) noexcept {
  if (track >= kTracks || strips_[track].mute == on) {
    return;
  }
  strips_[track].mute = on;
  mark_dirty(track);
  ramp_[track] = 0;
}

bool Graph::apply_strip(unsigned slot, unsigned field, float v, std::uint32_t ramp) noexcept {
  Strip& s = strips_[slot];
  const bool is_input = slot == kInputStrip;
  switch (field) {
    case graph_param::strip::Volume:
      if (!in_range(v, 0.0f, 100.0f)) return false;
      s.volume = v;
      break;
    case graph_param::strip::Pan:
      if (is_input || !in_range(v, 0.0f, 100.0f)) return false;
      s.pan = v;
      break;
    case graph_param::strip::Mute:
      if (v != 0.0f && v != 1.0f) return false;
      s.mute = v != 0.0f;
      break;
    case graph_param::strip::Solo:
      if (is_input || (v != 0.0f && v != 1.0f)) return false;
      s.solo = v != 0.0f;
      any_solo_ = false;
      for (unsigned t = 0; t < kTracks; ++t) {
        any_solo_ = any_solo_ || (strips_[t].active && strips_[t].solo);
      }
      mark_all_dirty();  // a solo changes what every other track is
      for (unsigned i = 0; i < kStrips; ++i) {
        ramp_[i] = ramp;
      }
      return true;
    case graph_param::strip::Bus:
      return in_range(v, 0.0f, 16.0f);  // accepted, ignored: the stem slot is the StemTap's profile (the model's kMaxOutputBus is 16)
    case graph_param::strip::Route:
      if (!in_range(v, 0.0f, static_cast<float>(route::kAll))) return false;
      s.route = static_cast<std::uint8_t>(as_uint(v) & (is_input ? (route::kAll & ~route::kUsb) : route::kAll));
      break;
    case graph_param::strip::SendA:
      if (!in_range(v, 0.0f, 100.0f)) return false;
      s.send_a = v;
      break;
    case graph_param::strip::SendB:
      if (!in_range(v, 0.0f, 100.0f)) return false;
      s.send_b = v;
      break;
    case graph_param::strip::XfGain:
      if (is_input || !in_range(v, 0.0f, 255.0f)) return false;
      s.xf_gain = static_cast<std::uint8_t>(as_uint(v));
      break;
    case graph_param::strip::Source: {
      if (is_input || !in_range(v, 0.0f, 3.0f)) return false;
      switch (as_uint(v)) {
        case 0:
          if (s.source == SourceKind::Sine || s.source == SourceKind::Noise) {
            s.tone.set_on(false);  // fades out, then the strip is skipped
          } else {
            s.source = SourceKind::None;
          }
          break;
        case 1:
          s.source = SourceKind::Sine;
          s.tone.set_on(true);
          break;
        case 2:
          s.source = SourceKind::Noise;
          s.tone.set_on(true);
          break;
        default:
          s.source = SourceKind::Feed;
          s.tone.set_on(false);
          break;
      }
      return true;
    }
    default:
      return false;
  }
  mark_dirty(slot);
  ramp_[slot] = ramp;
  return true;
}

bool Graph::apply_limiter_mode(float v) noexcept { return limiter_ != nullptr && limiter_->set_param(limiter::Mode, v); }

bool Graph::apply_master(unsigned field, float v, std::uint32_t ramp) noexcept {
  bool strips_too = false;
  switch (field) {
    case graph_param::master::Volume:
      if (!in_range(v, 0.0f, 100.0f)) return false;
      m_volume_ = v;
      break;
    case graph_param::master::Dry:
      if (!in_range(v, 0.0f, 100.0f)) return false;
      m_dry_ = v;
      strips_too = true;
      break;
    case graph_param::master::Reverb:
      if (!in_range(v, 0.0f, 100.0f)) return false;
      m_reverb_ = v;
      strips_too = true;
      break;
    case graph_param::master::Delay:
      if (!in_range(v, 0.0f, 100.0f)) return false;
      m_delay_ = v;
      strips_too = true;
      break;
    case graph_param::master::DryMute:
    case graph_param::master::ReverbMute:
    case graph_param::master::DelayMute:
    case graph_param::master::Limiter:
      if (v != 0.0f && v != 1.0f) return false;
      if (field == graph_param::master::DryMute) {
        m_dry_mute_ = v != 0.0f;
        strips_too = true;
      } else if (field == graph_param::master::ReverbMute) {
        m_reverb_mute_ = v != 0.0f;
        strips_too = true;
      } else if (field == graph_param::master::DelayMute) {
        m_delay_mute_ = v != 0.0f;
        strips_too = true;
      } else {
        m_limiter_ = v != 0.0f;  // the project's LIMITER switch: told to the limiter (OS-LIMIT), whose controls it puts in force; the base limiter is on either way
        if (limiter_ != nullptr) {
          (void)limiter_->set_param(limiter::Switch, v);
        }
      }
      break;
    case graph_param::master::CueLevel:
      if (!in_range(v, 0.0f, 100.0f)) return false;
      m_cue_ = v;
      break;
    case graph_param::master::OutMode:
      if (v != 0.0f && v != 1.0f) return false;
      out_mode_ = v != 0.0f ? OutMode::MainLCueR : OutMode::Stereo;
      break;
    case graph_param::master::UsbReturnLevel:  // OS-006e
      if (!in_range(v, 0.0f, 100.0f)) return false;
      m_usb_level_ = v;
      break;
    case graph_param::master::DjFilter:  // OS-DJ A2: the master DJ filter's knob and its glide; the stage switches itself off at 0
      if (!dj_filter_.set(v, ramp)) return false;
      dj_engage();
      return true;
    case graph_param::master::KillEq:  // OS-DJ A3: the kill EQ's mask; the stage ramps its gains and switches itself off when none is killed
      if (!kill_eq_.set(v)) return false;
      kill_engage();
      return true;
    case graph_param::master::ScratchMode:  // OS-DJ C1: the stage engages itself (and clears its bypass) when a ring is staged, and bypasses itself when its release has ended
      return scratch_stage_.set_mode(v, ramp);
    case graph_param::master::MetroMode:  // OS-METRO: not a level of the graph, nothing to recompute
      return metro_.set_mode(v);
    case graph_param::master::MetroLevel:
      return metro_.set_level(v);
    case graph_param::master::RetRouteA:
    case graph_param::master::RetRouteB:
      if (!in_range(v, 0.0f, 3.0f)) return false;
      ret_route_[field == graph_param::master::RetRouteA ? 0 : 1] = static_cast<std::uint8_t>(as_uint(v));
      break;
    case graph_param::master::VoiceSendsReset:  // OS-SENDFX: a project load forgets the last notes' instrument sends
      if (v != 1.0f) return false;
      reset_voice_sends();
      return true;
    case graph_param::master::LimiterThreshold:  // OS-LIMIT: the limiter's own controls (limiter:: numbers 2..4 in the order of these ids)
    case graph_param::master::LimiterAttack:
    case graph_param::master::LimiterRelease:
      return limiter_ != nullptr && limiter_->set_param(limiter::Threshold + (field - graph_param::master::LimiterThreshold), v);
    default:
      if (field >= sendfx::kFieldBase[0] && field < sendfx::kFieldBase[1] + sendfx::kFieldSpan) {  // OS-SENDFX: a parameter of a send effect
        const unsigned which = field >= sendfx::kFieldBase[1] ? 1u : 0u;
        const FxSlot& f = fx_[which];
        return f.param != nullptr && f.param(f.state, field - sendfx::kFieldBase[which], v);
      }
      return false;
  }
  recompute_master(ramp);
  if (strips_too) {
    mark_all_dirty();
    for (unsigned i = 0; i < kStrips; ++i) {
      ramp_[i] = ramp;
    }
  }
  return true;
}

void Graph::set_usb_shut(bool shut) noexcept {
  usb_shut_ = shut;
  usb_gain_.retarget(usb_target(), 0);
}

void Graph::recompute_master(std::uint32_t ramp) noexcept {
  master_gain_.retarget(curve_.q16(m_volume_), ramp);
  cue_gain_.retarget(curve_.q16(m_cue_), ramp);
  usb_gain_.retarget(usb_target(), ramp);  // OS-006e: the USB return's own level; the DRY level and DRY MUTE are not in it
  for (unsigned a = 0; a < 2u; ++a) {
    for (unsigned d = 0; d < 2u; ++d) {
      ret_gain_[a][d].retarget(((ret_route_[a] >> d) & 1u) != 0u ? kernel::kUnity : 0, ramp);
    }
  }
}

void Graph::recompute_strip(unsigned slot, std::uint32_t ramp) noexcept {
  Strip& s = strips_[slot];
  const bool is_input = slot == kInputStrip;
  const bool user_muted = s.mute;
  const bool solo_muted = any_solo_ && !is_input && !s.solo;
  const bool dry_muted = user_muted || solo_muted;
  const bool send_muted = user_muted || (solo_mutes_sends_ && solo_muted);
  // The master DRY level scales the tracks' faders; the input strip is not a track of the song, so it keeps its own level (OS-006e: DRY is 86 in every v7+ project, which lowered `mon on` by 5.6 dB). DRY MUTE still mutes it.
  const std::int32_t dry = m_dry_mute_ ? 0 : (is_input ? kernel::kUnity : curve_.q16(m_dry_));
  // OS-COREFX CF3: the tracker's channel x global volume; OS-PERFFX P2: times the Perform layer's VOLUME, which only cuts: (1 + v/100)^2 below 0, nothing at or above it (no strip headroom to boost into)
  float trk_f = s.trk_gain;
  if (s.perf_gain < 0) {
    const float k = static_cast<float>(100 + s.perf_gain) * 0.01f;
    trk_f *= k * k;
  }
  if (s.xf_gain != 255u) {  // OS-DJ A1: the DJ crossfader's cut (0..255, a cut only), where the Perform layer's is: the dry level and the sends, per event, never per sample
    trk_f *= static_cast<float>(s.xf_gain) * (1.0f / 255.0f);
  }
  const std::int32_t trk = trk_f == 1.0f ? kernel::kUnity : static_cast<std::int32_t>(trk_f * 65536.0f + 0.5f);
  s.level.retarget(dry_muted ? 0 : mul_q16(mul_q16(curve_.q16(s.volume), dry), trk), ramp);
  // Sends are pre-fader (the default; the project's SENDS setting makes them post-fader: the send's level times the strip's own fader, the master dry volume not in it) and follow the mixer's levels: the
  // send's own level (the strip's plus the instrument's of the note) times the master DELAY / REVERB level, silenced by that mute and by the track's mute, and by a solo of another track (see the header: "Solo"); here also closed by the routing bit.
  // OS-COREFX CF3 (D3): the tracker's channel x global volume belongs to the note's gain, so it scales the sends whether they are pre- or post-fader; only the fader is skipped by a pre-fader send.
  const std::int32_t fader = sends_post_ ? mul_q16(curve_.q16(s.volume), trk) : trk;
  // The routing bit gates the strip's own send; the instrument's is open whatever the route, which the default route (MAIN | USB) needs for an OG project to sound.
  // OS-PERFFX P3: the step FX 30 / 32 held replaces the instrument's send; the Perform layer's DELAY / REVERB is an offset of a full send added to the sum held to 100, and the result is held to 0..100 again (so
  // -100 closes the send and +100 opens it whole whatever the strip's own and the instrument's are). Like the instrument's, it is not gated by the routing bit.
  const float level_a = send_level(((s.route & route::kSndA) != 0u ? s.send_a : 0.0f) + static_cast<float>(s.step_send_a >= 0 ? s.step_send_a : s.inst_send_a), s.perf_send_a);
  const float level_b = send_level(((s.route & route::kSndB) != 0u ? s.send_b : 0.0f) + static_cast<float>(s.step_send_b >= 0 ? s.step_send_b : s.inst_send_b), s.perf_send_b);
  const std::int32_t a = send_muted || m_delay_mute_ ? 0 : mul_q16(mul_q16(curve_.q16(level_a), curve_.q16(m_delay_)), fader);
  const std::int32_t b = send_muted || m_reverb_mute_ ? 0 : mul_q16(mul_q16(curve_.q16(level_b), curve_.q16(m_reverb_)), fader);
  s.send_gain[0].retarget(a, ramp);
  s.send_gain[1].retarget(b, ramp);
  retarget_pan(s, is_input, ramp);
}

// The pan gains of a track strip alone: a pan-only change (drain) and the end of recompute_strip.
void Graph::recompute_pan(unsigned slot, std::uint32_t ramp) noexcept {
  retarget_pan(strips_[slot], false, ramp);
}

void Graph::retarget_pan(Strip& s, bool is_input, std::uint32_t ramp) noexcept {
  const PanGains pan = is_input ? PanGains{kernel::kUnity, kernel::kUnity} : pan_gains_q16(s.pan + s.pan_offset + s.pan_mod + static_cast<float>(s.perf_pan));  // (OS-PERFFX P2: the Perform layer's PANNING, in pan units, with the tracker's offsets; the law holds the sum to 0..100)
  for (unsigned d = 0; d < kDests; ++d) {
    const bool open = (s.route & kDestBit[d]) != 0u;
    s.dest_l[d].retarget(open ? pan.left : 0, ramp);
    s.dest_r[d].retarget(open ? pan.right : 0, ramp);
  }
  s.settled = false;
}

// Every gain of a strip at its target, no ramp: for a strip that carries no signal this block (see the header: "Gains that carry no signal do not ramp").
void Graph::settle(Strip& s) noexcept {
  if (s.settled) {
    return;
  }
  s.level.snap();
  s.send_gain[0].snap();
  s.send_gain[1].snap();
  for (unsigned d = 0; d < kDests; ++d) {
    s.dest_l[d].snap();
    s.dest_r[d].snap();
  }
  s.settled = true;
}

// ---- The block ---------------------------------------------------------------------------------------------------------------

std::uint32_t Graph::mix_extras(std::int16_t* out, bool silence, std::uint32_t block_start) noexcept {
  if (metro_.busy()) {
    metro_.mix_out(out, kBlk, silence, out_mode_ == OutMode::MainLCueR);
  }
  if (loop_.needs_service()) {
    // OS-LOOPER L2c: the overdub writes what the REC tap recorded this block (the planes are intact after run_taps): the REC bus, or MAIN for a RESAMPLE, as one channel or their mean, the tap's own law.
    const std::int16_t* in = nullptr;
    if (loop_.wants_input()) {
      const bool master = rec_.source() == RecTap::Source::Master;
      const std::int16_t* const l = master ? pl_main_[0] : pl_rec_[0];
      const std::int16_t* const r = master ? pl_main_[1] : pl_rec_[1];
      const RecTap::Channel c = rec_.channel();
      if (c == RecTap::Channel::Mix) {
        kernel::mono_mix(mono_, l, r, kBlk);
        in = mono_;
      } else {
        in = c == RecTap::Channel::Right ? r : l;
      }
    }
    loop_.service(out, kBlk, silence, mul_q16(curve_.entry(loop_.volume()), master_gain_.current()), rec_, in, cycles_, block_start);
  }
  return cycles_();
}

void Graph::render(const hal::audio::BlockIo& io) noexcept {
  hal::audio::BlockReport& report = *io.report;
  const bool silence = (io.flags & hal::audio::kFlagSilence) != 0u;

  // input: the RX block as planes, and its raw peak (|-32768| counts as 32768)
  const std::uint32_t t0 = cycles_();
  kernel::deinterleave2(in_[0], in_[1], io.in, kBlk);
  kernel::MeterAcc raw{0u, 0u};
  kernel::meter(in_[0], kBlk, raw);
  kernel::meter(in_[1], kBlk, raw);
  report.input_peak = raw.peak;
  const InputFeed feed = input_feed();
  ret_live_ = false;
  if ((feed == InputFeed::ReturnOnly) != usb_shut_) {  // the recorder's USB source on or off: the strip takes the stream over from the return's route, or gives it back, over a block (no step)
    set_usb_shut(!usb_shut_);
  }
  if (return_on_.load(std::memory_order_relaxed)) {
    add_return(feed);  // part of the input stage: the host's stream is pulled; it goes to MAIN by its own route (run_usb_return), or replaces the line-in in the strip (the recorder's USB source)
  } else if (feed == InputFeed::ReturnOnly) {
    for (std::uint32_t i = 0; i < kBlk; ++i) {  // no host is streaming: the stream alone is silence
      in_[0][i] = 0;
      in_[1][i] = 0;
    }
  }

  ret_live_ = ret_live_ && !silence;  // (pulled above even so: the ring must not overrun while the render is muted)
  const std::uint32_t t1 = cycles_();
  drain();
  const std::uint32_t t2 = cycles_();
  run_tracks(silence);
  run_input(silence);
  const std::uint32_t t3 = cycles_();
  run_buses();
  const std::uint32_t t4 = cycles_();
  run_chain();
  const std::uint32_t t5 = cycles_();
  meter_buses();  // the buses as they stand after their saturation (MAIN after the chain, when there is one), before the master level
  const std::uint32_t t6 = cycles_();
  run_levels();
  const std::uint32_t t7 = cycles_();
  kernel::interleave2(io.out, outp_[0], outp_[1], kBlk);  // the one conversion to the TX layout
  const std::uint32_t t8 = cycles_();
  run_taps(io.out);
  const std::uint32_t t9 = cycles_();
  // OS-METRO: the click goes into the TX block last, after every tap has read the block (metro.h): in no meter, take, stem or export. The CUE side is the right channel of the split line out (OUT MODE
  // MainLCueR); the Stereo line out has no CUE, so a click set to CUE plays on both channels, MAIN's. A muted render (the codec is muted, or recovering) drops it. OS-LOOPER: so does the loop of a looped take
  // (loop_player.h), after the click. In flash, once a block while either has something to do.
  std::uint32_t t10 = t9;
  if (metro_.busy() || loop_.needs_service()) {
    t10 = mix_extras(io.out, silence, t0);
  }

  report.stage_cycles[hal::audio::kStageInput] = t1 - t0;
  report.stage_cycles[hal::audio::kStageDrain] = t2 - t1;
  report.stage_cycles[hal::audio::kStageRender] = src_cycles_;
  report.stage_cycles[hal::audio::kStageStrips] = (t3 - t2) - src_cycles_;
  report.stage_cycles[hal::audio::kStageBuses] = t4 - t3;
  report.stage_cycles[hal::audio::kStageMaster] = (t5 - t4) + (t7 - t6);
  report.stage_cycles[hal::audio::kStageConvert] = (t8 - t7) + (t10 - t9);  // (the click and the loop are part of the output stage)
  report.stage_cycles[hal::audio::kStageTaps] = (t6 - t5) + (t9 - t8);
}

// One track at a time through the two shared scratch blocks: source, inserts, sends, then the strip pass (level and pan, MAIN first: two tracks that
// open MAIN go through together), then what the other buses and the taps take of it.
void Graph::run_tracks(bool silence) noexcept {
  meter_now_ = meter_mask_.load(std::memory_order_relaxed);
  stem_have_ = 0;
  src_cycles_ = 0;
  Pending pend[2];
  unsigned np = 0;
  for (unsigned t = 0; t < kTracks; ++t) {
    Strip& s = strips_[t];
    s.live = false;
    s.tap = false;
    if (!s.active) {
      settle(s);
      continue;
    }
    // Is there a block to make? A silent strip runs nothing, and its gains go to their targets (settle).
    const std::int16_t* fed = nullptr;
    bool make = false;
    switch (s.source) {
      case SourceKind::Sine:
      case SourceKind::Noise:
        if (silence) {
          s.tone.skip(kBlk);  // the sources stay on their timelines and fade in again when the mute lifts
          s.tone.reset_gain();
        } else if (s.tone.active()) {
          make = true;
        } else {
          s.tone.skip(kBlk);
        }
        break;
      case SourceKind::Feed:
        fed = feed_[t].load(std::memory_order_acquire);
        make = !silence && fed != nullptr;
        break;
      case SourceKind::None:
        break;
    }
    if (!make) {
      if (__builtin_expect((s.insert[0].tail | s.insert[1].tail) == 0u, 1)) {
        settle(s);
        continue;
      }
      fed = zero_;  // OS-INSFX S2: an effect that may still ring keeps the strip live: the source block is silence, through the inserts, sends and fader (the byte is clear unless the slot is bound and not bypassed)
    }
    s.live = true;
    std::int16_t* const block = scratch_[np];  // the free one: nothing is pending in it
    const std::uint32_t t_a = cycles_();
    if (fed != nullptr) {  // (a feed, or the tail's silence; a tone source leaves it null)
      copy_block(block, fed);
    } else {
      // The D2 voice-to-track conversion point: a float block, converted once to the track's Q15 block.
      if (s.source == SourceKind::Sine) {
        s.tone.generate(fl_[0], kBlk);
      } else {
        s.tone.generate_noise(fl_[0], kBlk, s.noise);
      }
      kernel::float_to_q15(block, fl_[0], kBlk);
    }
    src_cycles_ += cycles_() - t_a;

    for (unsigned k = 0; k < 2u; ++k) {  // the two inserts: reserved, bypassed until a contract installs one
      if (s.insert[k].fn != nullptr && !s.insert[k].bypass) {
        s.insert[k].fn(s.insert[k].state, block, kBlk);
      }
    }
    for (unsigned a = 0; a < 2u; ++a) {  // sends, PRE-fader: the block as the inserts left it
      if (!s.send_gain[a].closed()) {
        const Ramp r = s.send_gain[a].next_block();
        kernel::mac(acc_snd_[a], block, kBlk, r.start, r.end);
        b_snd_[a].dirty = true;
      }
    }
    if (s.level.closed()) {  // the fader is shut: nothing downstream of it (and no tap); the routes carry nothing, so they stand at their targets
      for (unsigned d = 0; d < kDests; ++d) {
        s.dest_l[d].snap();
        s.dest_r[d].snap();
      }
      continue;
    }
    s.settled = false;
    const Ramp lv = s.level.next_block();
    if (!s.dest_l[0].closed() || !s.dest_r[0].closed()) {
      // MAIN is the common route: the fader and the pan law in one pass over the block (kernel::gain_pan / gain_mac and their two-track forms).
      // A track whose two pan gains are the same ramp (the centre) adds into the one centre accumulator.
      const bool centre = s.dest_l[0].same_as(s.dest_r[0]);
      const Ramp rl = s.dest_l[0].closed() ? Ramp{0, 0} : s.dest_l[0].next_block();
      const Ramp rr = s.dest_r[0].closed() ? Ramp{0, 0} : s.dest_r[0].next_block();
      pend[np] = Pending{t, block, kernel::StripLane{block, lv, rl, rr}, centre};
      ++np;
      // OS-STEREO (c): the track's S (voices.h STEREO VOICES) through the same level ramp and pan law into MAIN's two sides, after the mono pass (the sums commute). A strip that got here is live, unmuted and open
      // (the closed-fader case continued above). An insert in the path processed M only, so S is not used. The call is in flash (one per stereo track); a mono track pays the one load.
      const std::int16_t* const side = side_[t].load(std::memory_order_relaxed);  // (g: see feed_side)
      if (__builtin_expect(side != nullptr, 0)) {
        side_pass(t, side, pend[np - 1u].lane);
      }
      if (np == 2u) {
        flush_main(pend, 2);
        np = 0;
      }
    } else {
      kernel::gain(block, block, kBlk, lv.start, lv.end);
      finish_track(t, block);
    }
  }
  if (np != 0u) {
    flush_main(pend, np);
  }
  // OS-ODD11: which strips carry nothing now, for the voices of the next block (VoicePool::set_gate). From the smoothers, after this block moved them: the level is tested first, so an audible strip costs one compare.
  std::uint16_t quiet = 0;
  for (unsigned t = 0; t < kAuditionTrack; ++t) {
    const Strip& q = strips_[t];
    if (q.active && q.level.closed() && q.send_gain[0].closed() && q.send_gain[1].closed()) {
      quiet = static_cast<std::uint16_t>(quiet | (1u << t));
    }
  }
  quiet_ = quiet;
}

// OS-STEREO (c): the side signal of a stereo voice on `track` through the strip's level ramp and pan gains (the lane the mono pass is given) into MAIN's two sides: L += S, R -= S. A strip whose source is not
// the voices' feed takes none; one with an insert in the path (it processed M only) takes S as much as the insert leaves of the dry signal: OS-INSFX S1, 255 - wet of each insert in the path, from the wet
// share it had at the start and has at the end of the block (set_insert_wet), so S comes back and goes away over one block, no step; a steady insert (wet 255) takes none, as before.
// In flash, one call per stereo track and block.
void Graph::side_pass(unsigned track, const std::int16_t* side, const kernel::StripLane& lane) noexcept {
  const Strip& s = strips_[track];
  if (s.source != SourceKind::Feed) {
    return;
  }
  std::uint32_t g0 = 255u;
  std::uint32_t g1 = 255u;
  for (unsigned k = 0; k < 2u; ++k) {
    if (s.insert[k].fn != nullptr && !s.insert[k].bypass) {
      g0 = g0 * (255u - s.insert[k].wet0) / 255u;
      g1 = g1 * (255u - s.insert[k].wet1) / 255u;
    }
  }
  if (g0 == 0u && g1 == 0u) {
    return;
  }
  kernel::GainRamp level = lane.level;
  if (g0 != 255u || g1 != 255u) {
    level.start = static_cast<std::int32_t>(static_cast<std::int64_t>(level.start) * g0 / 255);
    level.end = static_cast<std::int32_t>(static_cast<std::int64_t>(level.end) * g1 / 255);
  }
  kernel::gain_pan_st(side, level, lane.left, lane.right, acc_main_[0], acc_main_[1], kBlk);
  b_main_.dirty = true;
}

void Graph::set_insert_tail(unsigned track, unsigned slot, bool on) noexcept {
  if (track < kTracks && slot < 2u) {
    Insert& ins = strips_[track].insert[slot];
    ins.tail = (on && ins.fn != nullptr) ? 1u : 0u;  // (flash: a slot with no module never keeps a strip live: a render that writes the byte after a forced release cleared the slot is ignored)
  }
}

void Graph::set_insert_wet(unsigned track, unsigned slot, unsigned wet0, unsigned wet1) noexcept {
  if (track < kTracks && slot < 2u) {
    strips_[track].insert[slot].wet0 = static_cast<std::uint8_t>(wet0 > 255u ? 255u : wet0);
    strips_[track].insert[slot].wet1 = static_cast<std::uint8_t>(wet1 > 255u ? 255u : wet1);
  }
}

// The strip pass for the tracks waiting: both at once if they are of one kind (two centred, or two panned), else one by one.
void Graph::flush_main(Pending* p, unsigned n) noexcept {
  if (n == 2u && p[0].centre == p[1].centre) {
    if (p[0].centre) {
      kernel::gain_mac2(p[0].lane, p[1].lane, acc_main_c_, kBlk);
      b_main_.centre = true;
    } else {
      kernel::gain_pan2(p[0].lane, p[1].lane, acc_main_[0], acc_main_[1], kBlk);
      b_main_.dirty = true;
    }
  } else {
    for (unsigned k = 0; k < n; ++k) {
      if (p[k].centre) {
        kernel::gain_mac(p[k].lane, acc_main_c_, kBlk);
        b_main_.centre = true;
      } else {
        kernel::gain_pan(p[k].lane, acc_main_[0], acc_main_[1], kBlk);
        b_main_.dirty = true;
      }
    }
  }
  for (unsigned k = 0; k < n; ++k) {
    finish_track(p[k].track, p[k].block);
  }
}

// What follows the fader for a track whose post-fader block is `block`: the pan law into CUE and REC, its meter, its copy for the stems.
void Graph::finish_track(unsigned track, std::int16_t* block) noexcept {
  Strip& s = strips_[track];
  s.tap = true;
  if (!s.dest_l[1].closed()) {
    const Ramp r = s.dest_l[1].next_block();
    kernel::mac(acc_cue_[0], block, kBlk, r.start, r.end);
    b_cue_.dirty = true;
  }
  if (!s.dest_r[1].closed()) {
    const Ramp r = s.dest_r[1].next_block();
    kernel::mac(acc_cue_[1], block, kBlk, r.start, r.end);
    b_cue_.dirty = true;
  }
  if (!s.dest_l[2].closed()) {
    const Ramp r = s.dest_l[2].next_block();
    kernel::mac(acc_rec_[0], block, kBlk, r.start, r.end);
    b_rec_.dirty = true;
  }
  if (!s.dest_r[2].closed()) {
    const Ramp r = s.dest_r[2].next_block();
    kernel::mac(acc_rec_[1], block, kBlk, r.start, r.end);
    b_rec_.dirty = true;
  }
  const unsigned channel = meter_ch::kTrack0 + track;
  if (((meter_now_ >> channel) & 1u) != 0u) {
    kernel::meter(block, kBlk, meters_.acc(channel));
  }
  if (track < stem_src::kTracks && (s.route & route::kUsb) != 0u && stems_.enabled()) {
    copy_block(stems_.stage(track), block);
    stem_have_ |= 1u << track;
  }
}

// The USB return of this block: pulled even while the render is muted (the ring must not overrun) into ret_. It reaches the outputs by its own route (run_usb_return, run_levels), and
// while the input strip is made of it alone (the recorder's USB source) the strip takes the stream in place of the line-in and the route's gain ramps to 0.
void Graph::add_return(InputFeed feed) noexcept {
  if (return_reader_ == nullptr || return_pull_ == nullptr) {
    return;
  }
  return_pull_(return_reader_, ret_[0], ret_[1], kBlk);  // silence for what the ring could not supply (counted by the reader)
  if (feed == InputFeed::ReturnOnly) {
    copy_block(in_[0], ret_[0]);
    copy_block(in_[1], ret_[1]);
  }
  ret_live_ = true;  // (in ReturnOnly the route's gain is on its way to 0, or there: run_usb_return)
}

// The input strip: level, routing bits, sends; the two channels stay apart.
void Graph::run_input(bool silence) noexcept {
  Strip& in = strips_[kInputStrip];
  in.live = !silence;
  in.tap = false;
  if (!in.live) {
    settle(in);
    return;
  }
  std::int32_t(*const accs[3])[kBlk] = {acc_main_, acc_cue_, acc_rec_};
  Bus* const buses[3] = {&b_main_, &b_cue_, &b_rec_};
  if (!in.send_gain[0].closed() || !in.send_gain[1].closed()) {
    kernel::mono_mix(mono_, in_[0], in_[1], kBlk);
    for (unsigned a = 0; a < 2u; ++a) {
      if (!in.send_gain[a].closed()) {
        const Ramp r = in.send_gain[a].next_block();
        kernel::mac(acc_snd_[a], mono_, kBlk, r.start, r.end);
        b_snd_[a].dirty = true;
      }
    }
  }
  if (in.level.closed()) {
    for (unsigned d = 0; d < kDests; ++d) {
      in.dest_l[d].snap();
      in.dest_r[d].snap();
    }
    return;
  }
  in.settled = false;
  const Ramp lv = in.level.next_block();
  kernel::gain(in_[0], in_[0], kBlk, lv.start, lv.end);
  kernel::gain(in_[1], in_[1], kBlk, lv.start, lv.end);
  in.tap = true;
  for (unsigned d = 0; d < kDests; ++d) {
    if (!in.dest_l[d].closed()) {
      const Ramp r = in.dest_l[d].next_block();
      kernel::mac(accs[d][0], in_[0], kBlk, r.start, r.end);
      buses[d]->dirty = true;
    }
    if (!in.dest_r[d].closed()) {
      const Ramp r = in.dest_r[d].next_block();
      kernel::mac(accs[d][1], in_[1], kBlk, r.start, r.end);
      buses[d]->dirty = true;
    }
  }
}

void Graph::finish_bus(Bus& bus, std::int16_t (*planes)[kBlk], std::int32_t (*acc)[kBlk], unsigned channels) noexcept {
  if (bus.dirty) {
    for (unsigned c = 0; c < channels; ++c) {
      kernel::sat_clear(planes[c], acc[c], kBlk);  // the one saturation of the bus (D2); the accumulator is empty again
    }
    bus.plane = true;
    bus.dirty = false;
  } else if (bus.plane) {
    for (unsigned c = 0; c < channels; ++c) {
      for (std::uint32_t i = 0; i < kBlk; ++i) {
        planes[c][i] = 0;
      }
    }
    bus.plane = false;
  }
}

void Graph::run_buses() noexcept {
  std::int32_t(*const accs[2])[kBlk] = {acc_main_, acc_cue_};
  Bus* const buses[2] = {&b_main_, &b_cue_};
  chain_on_ = chain_active();
  for (unsigned a = 0; a < 2u; ++a) {
    finish_bus(b_snd_[a], &pl_snd_[a], &acc_snd_[a], 1);
    const FxSlot& f = fx_[a];
    if (f.fn != nullptr) {
      // A slot with a processor runs every block, bypassed or not. BYPASS CLOSES THE INPUT: the effect is handed silence, so what it has taken in
      // rings out through its return (a delay's repeats, a reverb's tail), and nothing new goes in. This is the decided behaviour for the real
      // effects of OS-005e, which take effect when they arrive (the doubles of 005b are memoryless: their tail is nothing); a slot without a
      // processor has no return at all.
      f.fn(f.state, f.bypass ? zero_ : pl_snd_[a], pl_ret_[a][0], pl_ret_[a][1], kBlk);
      for (unsigned d = 0; d < 2u; ++d) {
        if (!ret_gain_[a][d].closed()) {
          const Ramp r = ret_gain_[a][d].next_block();
          kernel::mac(accs[d][0], pl_ret_[a][0], kBlk, r.start, r.end);
          kernel::mac(accs[d][1], pl_ret_[a][1], kBlk, r.start, r.end);
          buses[d]->dirty = true;
        }
      }
    }
  }
  if (chain_on_) {
    // The master chain is float: MAIN goes there straight from its accumulators, not through int16 (D2: the final float_to_q15 is the only
    // saturation of the bus; a sum beyond full scale reaches the stages whole).
    kernel::acc_to_float_main(fl_[0], fl_[1], acc_main_[0], acc_main_[1], acc_main_c_, kBlk);
    b_main_.dirty = false;
    b_main_.centre = false;
  } else if (b_main_.dirty || b_main_.centre) {
    kernel::sat_clear_main(pl_main_[0], pl_main_[1], acc_main_[0], acc_main_[1], b_main_.centre ? acc_main_c_ : nullptr, kBlk);
    b_main_.plane = true;
    b_main_.dirty = false;
    b_main_.centre = false;
  } else if (b_main_.plane) {
    for (std::uint32_t i = 0; i < kBlk; ++i) {
      pl_main_[0][i] = 0;
      pl_main_[1][i] = 0;
    }
    b_main_.plane = false;
  }
  finish_bus(b_cue_, pl_cue_, acc_cue_, 2);
  finish_bus(b_rec_, pl_rec_, acc_rec_, 2);
}

// The chain: float only inside it, one round trip per block, and only when a stage is active: with every slot bypassed (all of them, in 005b) the MAIN
// bus goes through untouched, bit for bit.
void Graph::run_chain() noexcept {
  main_pre_ = false;
  if (!chain_on_) {
    return;
  }
  for (unsigned i = 0; i < kMasterStages; ++i) {  // kMasterChain order
    if (master_[i].fn != nullptr && !master_[i].bypass) {
      if (i == static_cast<unsigned>(MasterStage::Limiter)) {
        // OS-LIMIT: MAIN is metered as it arrives at the limiter (saturated to int16 for the meter only), so its clip count is the overload the limiter had to catch.
        main_pre_ = true;
        for (unsigned c = 0; c < 2u; ++c) {
          if (((meter_now_ >> (meter_ch::kMainL + c)) & 1u) != 0u) {
            kernel::float_to_q15(pl_main_[c], fl_[c], kBlk);
            kernel::meter(pl_main_[c], kBlk, meters_.acc(meter_ch::kMainL + c));
          }
        }
      }
      master_[i].fn(master_[i].state, fl_[0], fl_[1], kBlk);
    }
  }
  kernel::float_to_q15(pl_main_[0], fl_[0], kBlk);  // the bus's one saturation
  kernel::float_to_q15(pl_main_[1], fl_[1], kBlk);
  b_main_.plane = true;  // a stage may have made sound from silence (a reverb tail)
}

void Graph::meter_buses() noexcept {
  const std::uint32_t mask = meter_now_;
  auto wanted = [mask](unsigned channel) { return ((mask >> channel) & 1u) != 0u; };
  if (strips_[kInputStrip].tap) {
    if (wanted(meter_ch::kInL)) kernel::meter(in_[0], kBlk, meters_.acc(meter_ch::kInL));
    if (wanted(meter_ch::kInR)) kernel::meter(in_[1], kBlk, meters_.acc(meter_ch::kInR));
  }
  if (b_main_.plane && !main_pre_) {  // (with a limiter in the chain MAIN was metered before it)
    if (wanted(meter_ch::kMainL)) kernel::meter(pl_main_[0], kBlk, meters_.acc(meter_ch::kMainL));
    if (wanted(meter_ch::kMainR)) kernel::meter(pl_main_[1], kBlk, meters_.acc(meter_ch::kMainR));
  }
  if (b_cue_.plane) {
    if (wanted(meter_ch::kCueL)) kernel::meter(pl_cue_[0], kBlk, meters_.acc(meter_ch::kCueL));
    if (wanted(meter_ch::kCueR)) kernel::meter(pl_cue_[1], kBlk, meters_.acc(meter_ch::kCueR));
  }
  if (b_rec_.plane) {
    if (wanted(meter_ch::kRecL)) kernel::meter(pl_rec_[0], kBlk, meters_.acc(meter_ch::kRecL));
    if (wanted(meter_ch::kRecR)) kernel::meter(pl_rec_[1], kBlk, meters_.acc(meter_ch::kRecR));
  }
  for (unsigned a = 0; a < 2u; ++a) {
    if (b_snd_[a].plane && wanted(meter_ch::kSndA + a)) {
      kernel::meter(pl_snd_[a], kBlk, meters_.acc(meter_ch::kSndA + a));
    }
  }
}

void Graph::run_levels() noexcept {
  const Ramp mg = master_gain_.next_block();
  if (b_main_.plane && !(mg.start == kernel::kUnity && mg.end == kernel::kUnity)) {
    kernel::gain(pl_main_[0], pl_main_[0], kBlk, mg.start, mg.end);
    kernel::gain(pl_main_[1], pl_main_[1], kBlk, mg.start, mg.end);
  }
  const Ramp cg = cue_gain_.next_block();
  if (b_cue_.plane && !(cg.start == kernel::kUnity && cg.end == kernel::kUnity)) {
    kernel::gain(pl_cue_[0], pl_cue_[0], kBlk, cg.start, cg.end);
    kernel::gain(pl_cue_[1], pl_cue_[1], kBlk, cg.start, cg.end);
  }
  // The pair the outputs are made of: MAIN, or MAIN with the USB return (OS-006e) added HERE, to what is heard, and to nothing else: pl_main_ (so the stems' Master L / R, RecTap::Source::Master
  // and the MAIN meters) stays without it, so a host that monitors the Tracker's capture while it plays into the Tracker builds no loop. It bypasses the limiter; the sum saturates at full
  // scale, through the (empty) MAIN accumulators. Gain: the return's own times the master level, ramped from the product of the starts to that of the ends. The gain is snapped while
  // there is nothing to hear (no host, a muted render), so that it comes back at the level it was asked for, not on an interrupted ramp.
  std::int16_t(*main)[kBlk] = pl_main_;
  ret_heard_ = ret_live_ && !usb_gain_.closed();
  if (ret_heard_) {
    const Ramp ur = usb_gain_.next_block();
    const std::int32_t g0 = mul_q16(ur.start, mg.start);
    const std::int32_t g1 = mul_q16(ur.end, mg.end);
    for (unsigned c = 0; c < 2u; ++c) {
      kernel::mac(acc_main_[c], ret_[c], kBlk, g0, g1);
      kernel::mac(acc_main_[c], pl_main_[c], kBlk, kernel::kUnity, kernel::kUnity);  // MAIN as it is (exact at unity; zeros when it has nothing)
      kernel::sat_clear(pl_out_[c], acc_main_[c], kBlk);
    }
    main = pl_out_;
  } else {
    usb_gain_.snap();
  }
  if (out_mode_ == OutMode::Stereo) {
    outp_[0] = main[0];
    outp_[1] = main[1];
  } else {
    kernel::mono_mix(pl_out_[0], main[0], main[1], kBlk);  // (main may be pl_out_ itself: each pair is read before it is written)
    kernel::mono_mix(pl_out_[1], pl_cue_[0], pl_cue_[1], kBlk);
    outp_[0] = pl_out_[0];
    outp_[1] = pl_out_[1];
  }
}

void Graph::run_taps(const std::int16_t* out_block) noexcept {
  const bool heard = b_main_.plane || b_cue_.plane || ret_heard_;
  if (heard && ((meter_now_ >> meter_ch::kMasterL) & 1u) != 0u) {
    kernel::meter(outp_[0], kBlk, meters_.acc(meter_ch::kMasterL));
  }
  if (heard && ((meter_now_ >> meter_ch::kMasterR) & 1u) != 0u) {
    kernel::meter(outp_[1], kBlk, meters_.acc(meter_ch::kMasterR));
  }
  meters_.end_block();

  if (scope_on_.load(std::memory_order_relaxed)) {
    mem_.scope.write(out_block, hal::audio::kBlockSamples);  // what is heard, interleaved
  }
  bool master_mono = false;  // mono_ holds the master pair's mono sum
  if (spectrum_on_.load(std::memory_order_relaxed)) {
    kernel::mono_mix(mono_, outp_[0], outp_[1], kBlk);
    mem_.spectrum.write(mono_, kBlk);
    master_mono = true;
  }
  if (app_tap_ != nullptr) {  // OS-H2a: the apps' tap (app_tap.h): one relaxed load while it is off
    const AppTap::Source src = app_tap_->source();
    if (src != AppTap::Source::Off) {
      const std::uint32_t t_tap = cycles_();
      if (src == AppTap::Source::Input) {
        kernel::mono_mix(mono_, in_[0], in_[1], kBlk);  // (the input mono send of run_input is long done with mono_)
      } else if (!master_mono) {
        kernel::mono_mix(mono_, outp_[0], outp_[1], kBlk);
      }
      app_tap_->push(mono_);
      app_tap_->note_cost(cycles_() - t_tap);
    }
  }

  if (stems_.enabled()) {
    const std::int16_t* src[usb::kSlotSources];
    for (unsigned t = 0; t < stem_src::kTracks; ++t) {
      src[t] = ((stem_have_ >> t) & 1u) != 0u ? stems_.stage(t) : nullptr;
    }
    src[stem_src::kMasterL] = b_main_.plane ? pl_main_[0] : nullptr;
    src[stem_src::kMasterR] = b_main_.plane ? pl_main_[1] : nullptr;
    src[stem_src::kSendA] = b_snd_[0].plane ? pl_snd_[0] : nullptr;
    src[stem_src::kSendB] = b_snd_[1].plane ? pl_snd_[1] : nullptr;
    stems_.push(src);
  } else {
    stems_.push(nullptr);  // keeps its phase bookkeeping (a disabled tap is one flag)
  }
  const bool master = rec_.source() == RecTap::Source::Master;  // a resample of what is heard: MAIN after the master level (OS-011c)
  rec_.push(master ? pl_main_[0] : pl_rec_[0], master ? pl_main_[1] : pl_rec_[1], kBlk);
}

}  // namespace o3os::audio
