// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/audio/module_voices.h>

#include <o3os/audio/module_q15.h>

#include <o3os/core/fast_math.h>
#include <o3os/module/clock_state.h>
#include <o3os/module/image.h>
#include <o3os/module/key_state.h>
#include <o3os/module/view.h>

#include <cmath>
#include <cstring>

#if defined(__ARM_FP) && defined(__ARM_FEATURE_SAT)
#include <arm_acle.h>
#endif

namespace o3os::audio {

namespace {

// The perf cell's seqlock data accesses (as audio/status.cpp does it): relaxed, ordered by the sequence number and a fence on each side (a single core: the writer is the audio interrupt that preempts
// the reader). ThreadSanitizer does not model fences, so under it the same protocol runs with every data access sequentially consistent, which it does model and which is correct too.
#if defined(__SANITIZE_THREAD__)
constexpr std::memory_order kPerfData = std::memory_order_seq_cst;
inline void perf_fence_release() noexcept {}
inline void perf_fence_acquire() noexcept {}
#else
constexpr std::memory_order kPerfData = std::memory_order_relaxed;
inline void perf_fence_release() noexcept { std::atomic_thread_fence(std::memory_order_release); }
inline void perf_fence_acquire() noexcept { std::atomic_thread_fence(std::memory_order_acquire); }
#endif

// ---- the output side of render() (OS-303PERF M4; both are folded into render(), which runs from SRAM_L) ---------------------------------------------------------------
// A word at a time: the block is 4-byte aligned (attach_scratch takes 16-byte memory and the blocks follow the float scratch at multiples of 128 bytes), so the 64 halfword stores are 32 word stores.
static_assert((ModuleVoices::kBlock * 4u) % 4u == 0u && (ModuleVoices::kBlock * 2u) % 16u == 0u && ModuleVoices::kBlock % 8u == 0u, "the instances' blocks start on a word and are a whole number of 8-halfword steps");
O3OS_AUDIO_INLINE void zero_block(std::int16_t* block) noexcept {
  std::int16_t* const p = static_cast<std::int16_t*>(__builtin_assume_aligned(block, 4));
  const std::uint32_t zero = 0u;
  for (std::uint32_t k = 0; k < ModuleVoices::kBlock; k += 8u) {  // (memcpy of a word: a STR; never a call to memset, which SRAM_L may not make)
    std::memcpy(p + k, &zero, 4);
    std::memcpy(p + k + 2u, &zero, 4);
    std::memcpy(p + k + 4u, &zero, 4);
    std::memcpy(p + k + 6u, &zero, 4);
  }
}

float host_exp2(float x) { return core::exp2_fast(x); }
float host_log2(float x) { return core::log2_fast(x); }
// sin and cos for a module's set-up code (coefficients: a few calls at init and at a parameter change): libm's sinf / cosf bring the argument reduction for any
// float, about 4 KB of flash, for a service that is asked about corner frequencies. The argument is brought into [-pi, pi] (2 pi as two floats: k turns cost no
// precision), folded into [-pi/2, pi/2] and a Taylor series of x (relative error under 1e-7, the small angles of a 7 Hz filter included) does the rest. Absolute error
// under 1e-6 for |x| < 1000 rad; NaN and infinity give 0.
float host_sin(float x) { return core::sin_fast(x); }
float host_cos(float x) { return core::cos_fast(x); }
void host_log_none(const char*) {}

// ABI 1.12 (OS-INSFX-ABI2): get_clock (A3) answers what the engine wrote at the last block boundary (module/clock_state.h): the tempo that plays, the beat as of this block's first sample and the flags, and
// returns 0. With no source behind it (nothing written: boot, a test) it says "stopped, the project tempo" (12000 = 120 BPM, beat 0, no flags) and returns 1. track_level is the stub of A6: 0. A caller that
// set a size below sizeof(o3m_clock) gets the members that fit, as get_transport does. No allocation, no loop: four atomic loads and a float multiply (the Q16.16 beat times 2^-16 is exact).
std::int32_t host_get_clock(o3m_clock* out) {
  if (out == nullptr) {
    return 1;
  }
  o3m_clock c = {static_cast<std::uint32_t>(sizeof(o3m_clock)), 0u, 12000u, 0.0f};
  std::uint32_t flags = 0;
  std::uint32_t tempo = 0;
  std::uint32_t beat = 0;
  const bool live = module::g_clock.read(flags, tempo, beat);
  if (live) {
    c.flags = flags & (O3M_CLOCK_PLAYING | O3M_CLOCK_STARTED | O3M_CLOCK_WRAPPED);
    c.tempo_x100 = tempo;
    c.beat = static_cast<float>(beat) * (1.0f / 65536.0f);
  }
  const std::uint32_t want = out->size < sizeof(o3m_clock) ? out->size : static_cast<std::uint32_t>(sizeof(o3m_clock));
  const std::uint32_t keep = out->size;
  std::memcpy(out, &c, want);
  out->size = keep < sizeof(o3m_clock) ? keep : static_cast<std::uint32_t>(sizeof(o3m_clock));
  return live ? 0 : 1;
}
float host_track_level(std::uint32_t track) { return module::g_keys.level(track); }  // OS-INSFX-ABI2 A6: the key store (module/key_state.h), render context

}  // namespace

ModuleVoices::ModuleVoices() noexcept {
  for (unsigned i = 0; i < kModuleSlots; ++i) {
    slot_[i].store(kNone, std::memory_order_relaxed);
    gate_[i].store(0, std::memory_order_relaxed);
  }
  for (unsigned t = 0; t < kModuleTracks; ++t) {
    owner_[t] = kNone;
  }
  host_.abi_major = O3M_ABI_MAJOR;
  host_.abi_minor = O3M_ABI_MINOR;
  host_.size = sizeof(o3m_host);
  host_.sample_rate = 44100.0f;
  host_.log = &host_log_none;
  host_.exp2_f = &host_exp2;
  host_.log2_f = &host_log2;
  host_.sin_f = &host_sin;
  host_.cos_f = &host_cos;
  host_.get_clock = &host_get_clock;      // ABI 1.12 (OS-INSFX-ABI2 A3 and A6): both live, track_level below is the key store
  host_.track_level = &host_track_level;
  host_.draw = module::draw_api();  // ABI 1.1 (OS-012b): the restricted draw API of the views (module/view.h); a module only gets a canvas inside view_draw()
  refresh_host();
}

void ModuleVoices::refresh_host() noexcept {
  for (unsigned k = 0; k < kMaxRateShift; ++k) {
    host_rate_[k] = host_;
    host_rate_[k].sample_rate = host_.sample_rate / static_cast<float>(2u << k);
  }
}

void ModuleVoices::set_hooks(const Hooks& hooks) noexcept {
  hooks_ = hooks;
  host_.log = hooks.log != nullptr ? hooks.log : &host_log_none;
  refresh_host();
}

bool ModuleVoices::attach_scratch(void* mem, std::uint32_t bytes) noexcept {
  if (mem == nullptr || bytes < kScratchBytes || (reinterpret_cast<std::uintptr_t>(mem) & 15u) != 0u) {
    return false;
  }
  std::uint8_t* base = static_cast<std::uint8_t*>(mem);
  scratch_ = reinterpret_cast<float*>(base);
  std::int16_t* blocks = reinterpret_cast<std::int16_t*>(base + kBlock * 4u);
  for (unsigned i = 0; i < kMaxModuleInstances; ++i) {
    block_[i] = blocks + i * kBlock;
    std::memset(block_[i], 0, kBlock * 2u);
  }
  return true;
}

ModuleVoices::Bind ModuleVoices::bind(unsigned slot, const o3m_module& mod, std::uint16_t fault_id, void* state, const std::uint8_t* params,
                                      std::uint32_t param_len) noexcept {
  if (scratch_ == nullptr) {
    return Bind::NoScratch;
  }
  if (slot >= kModuleSlots) {
    return Bind::BadSlot;
  }
  if (slot_[slot].load(std::memory_order_relaxed) != kNone) {
    return Bind::SlotTaken;
  }
  if (state == nullptr || (reinterpret_cast<std::uintptr_t>(state) & 15u) != 0u) {
    return Bind::BadState;
  }
  unsigned idx = kMaxModuleInstances;
  for (unsigned i = 0; i < kMaxModuleInstances; ++i) {
    if (in_[i].mod == nullptr) {
      idx = i;
      break;
    }
  }
  if (idx == kMaxModuleInstances) {
    return Bind::NoInstance;
  }
  Instance& in = in_[idx];
  in.fault_id = fault_id;
  const std::uint16_t prev = enter(in);
  const int rc = mod.init(state, &host_);
  if (rc != 0) {
    leave(prev);
    return Bind::InitFailed;
  }
  if (params != nullptr) {
    const std::uint32_t n = mod.param_count < param_len / 4u ? mod.param_count : param_len / 4u;
    for (std::uint32_t i = 0; i < n; ++i) {
      float v;
      std::memcpy(&v, params + 4u * i, 4);
      if (v == v && v > -1.0e30f && v < 1.0e30f) {
        mod.set_param(state, i, v);
      }
    }
  }
  leave(prev);
  in.mod = &mod;
  in.state = state;
  in.slot = static_cast<std::uint8_t>(slot);
  in.track = kNone;
  in.held = 0;
  in.held_overlap = 0;
  in.active = 0;
  in.last_note = 0;
  in.pitch = mod.abi_minor >= module::kPitchAbiMinor ? mod.pitch : nullptr;  // OS-MODPITCH: the members past byte 64 exist only in a descriptor of 1.8 (a 1.7 module's is 64 bytes: nothing past it is read)
  in.ramp = PitchRamp{};
  in.ramp_on = 0;
  in.shift = 0;  // OS-MODRATE: a bind is FULL (the host_ table it was inited with); the instrument's RATE is set after, set_rate() (ModuleUi::sync_slots)
  in.ph = 0;
  in.hold = 0.0f;
  in.rating = 0;
  in.gov.reset();  // OS-GOV: a reload starts the instance cold and unmuted
  in.acc_render = in.acc_out = in.acc_zero = 0;  // OS-303PERF: and with no measure. (The render does not add to the cell of an instance that is not published, but a `diag cpu reset` it serves clears every cell, this one too:
                                                  // the two clear() can meet, and both write only zeros and leave the sequence number even; see PerfCell)
  in.perf.clear();
  in.ready.store(1, std::memory_order_release);
  gate_[slot].store(0, std::memory_order_relaxed);  // (a bind opens the gate: the slot's instrument is the module's)
  slot_[slot].store(static_cast<std::uint8_t>(idx), std::memory_order_release);
  return Bind::Ok;
}

void ModuleVoices::retire_all() noexcept {
  bool any = false;
  for (unsigned i = 0; i < kMaxModuleInstances; ++i) {
    Instance& in = in_[i];
    if (in.mod != nullptr && in.ready.load(std::memory_order_relaxed) != 0u) {
      in.ready.store(0, std::memory_order_release);
      slot_[in.slot].store(kNone, std::memory_order_release);
      any = true;
    }
  }
  if (any) {
    retire_at_ = blocks_.load(std::memory_order_acquire);
    pending_ = true;
  }
}

bool ModuleVoices::quiescent() const noexcept {
  return !pending_ || static_cast<std::uint32_t>(blocks_.load(std::memory_order_acquire) - retire_at_) >= 2u;
}

void ModuleVoices::release_all() noexcept {
  for (unsigned i = 0; i < kMaxModuleInstances; ++i) {
    Instance& in = in_[i];
    if (in.mod != nullptr && in.ready.load(std::memory_order_relaxed) == 0u) {
      const std::uint16_t prev = enter(in);
      in.mod->shutdown(in.state);
      leave(prev);
      in.mod = nullptr;
      in.state = nullptr;
      in.track = kNone;
    }
  }
  pending_ = false;
}

unsigned ModuleVoices::rating_index(unsigned slot) const noexcept {
  for (unsigned i = 0; i < kMaxModuleInstances; ++i) {
    if (in_[i].mod != nullptr && in_[i].rating != 0u && in_[i].slot == slot) {
      return i;
    }
  }
  return kMaxModuleInstances;
}

unsigned ModuleVoices::rate(unsigned slot) const noexcept {
  const std::uint8_t idx = slot < kModuleSlots ? slot_[slot].load(std::memory_order_acquire) : kNone;
  return idx != kNone ? in_[idx].shift : 0u;
}

ModuleVoices::Rate ModuleVoices::set_rate(unsigned slot, unsigned shift, const std::uint8_t* params, std::uint32_t param_len, bool render_off) noexcept {
  if (slot >= kModuleSlots || shift > kMaxRateShift) {
    return Rate::Failed;
  }
  unsigned idx = rating_index(slot);
  if (idx == kMaxModuleInstances) {
    const std::uint8_t b = slot_[slot].load(std::memory_order_acquire);
    if (b == kNone) {
      return Rate::Failed;
    }
    idx = b;
  }
  Instance& in = in_[idx];
  const std::uint32_t now = blocks_.load(std::memory_order_acquire);
  if (in.rating == 0u) {
    if (in.shift == shift) {
      return Rate::Done;
    }
    if (shift != 0u && !rate_capable(*in.mod)) {
      return Rate::Failed;
    }
    in.want = static_cast<std::uint8_t>(shift);
    in.rating = kRatePending;  // a sounding note is never cut: nothing changes until the instance is idle
  } else {
    in.want = static_cast<std::uint8_t>(shift);  // (the latest ask wins)
  }
  if (in.rating == kRatePending) {
    if (in.want == in.shift) {  // asked back to what it is: nothing to do
      in.rating = 0;
      return Rate::Done;
    }
    // Idle: no note sounds and no tail rings (`active`, the render's: a byte read, as info() reads it).
    if (!render_off && in.active != 0u) {
      return Rate::Busy;
    }
    // Shut the gate (nothing new starts; the note path asks routed()). A note the render started between the idle test and here is caught below.
    const std::uint8_t was = gate_[slot].exchange(1, std::memory_order_acq_rel);
    in.regate = was == 0u ? 1u : 0u;
    in.rating = kRateClosing;
    in.at = now;
  }
  if (in.rating == kRateClosing) {
    if (!render_off) {
      if (static_cast<std::uint32_t>(now - in.at) < 1u) {
        return Rate::Busy;  // (the render serves the commands queued before the gate shut)
      }
      if (in.active != 0u) {  // a note began in that window: it plays out at the old rate, the change waits again
        gate_[slot].store(in.regate != 0u ? std::uint8_t{0} : std::uint8_t{1}, std::memory_order_release);
        in.rating = kRatePending;
        return Rate::Busy;
      }
      // Idle for good: queue the Release that frees the tracks the instance still owns (a held note whose sound is gone); the render serves it at its next block start.
      const Command c{Cmd::Release, static_cast<std::uint8_t>(slot), 0, 0, 0, 0, 0.0f};
      if (!ring_.push(c)) {
        return Rate::Busy;
      }
    }
    in.rating = kRateReleasing;
    in.at = now;
  }
  if (in.rating == kRateReleasing) {
    if (!render_off && static_cast<std::uint32_t>(now - in.at) < 1u) {
      return Rate::Busy;
    }
    in.ready.store(0, std::memory_order_release);  // withdrawn as retire_all() does: the render skips it, the note path finds no binding
    slot_[slot].store(kNone, std::memory_order_release);
    retire_at_ = now;
    pending_ = true;
    in.at = now;
    in.rating = kRateWithdrawn;
  }
  if (!render_off && static_cast<std::uint32_t>(now - in.at) < 2u) {
    return Rate::Busy;
  }
  // Clear of the render: the module restarts at the new rate, as a bind does.
  const o3m_module& mod = *in.mod;
  const std::uint16_t prev = enter(in);
  mod.shutdown(in.state);
  std::memset(in.state, 0, mod.state_bytes);
  const int rc = mod.init(in.state, &host_at(in.want));
  if (rc == 0 && params != nullptr) {
    const std::uint32_t n = mod.param_count < param_len / 4u ? mod.param_count : param_len / 4u;
    for (std::uint32_t i = 0; i < n; ++i) {
      float v;
      std::memcpy(&v, params + 4u * i, 4);
      if (v == v && v > -1.0e30f && v < 1.0e30f) {
        mod.set_param(in.state, i, v);
      }
    }
  }
  leave(prev);
  in.rating = 0;
  if (rc != 0) {  // the module refused its own restart: the instance is gone (its state was shut down above)
    in.mod = nullptr;
    in.state = nullptr;
    return Rate::Failed;
  }
  in.shift = in.want;
  in.track = kNone;
  in.held = 0;
  in.held_overlap = 0;
  in.active = 0;
  in.last_note = 0;
  in.ph = 0;
  in.hold = 0.0f;
  in.ramp = PitchRamp{};
  in.ramp_on = 0;
  in.gov.reset();
  in.ready.store(1, std::memory_order_release);
  gate_[slot].store(in.regate != 0u ? std::uint8_t{0} : std::uint8_t{1}, std::memory_order_release);
  slot_[slot].store(static_cast<std::uint8_t>(idx), std::memory_order_release);
  return Rate::Done;
}

void ModuleVoices::set_gate(unsigned slot, bool open) noexcept {
  if (slot >= kModuleSlots) {
    return;
  }
  const unsigned ri = rating_index(slot);
  if (ri != kMaxModuleInstances && in_[ri].rating != kRatePending) {  // OS-MODRATE: a RATE change holds the gate shut and puts it back as the UI last asked (set_rate)
    in_[ri].regate = open ? 1u : 0u;
    return;
  }
  const std::uint8_t was = gate_[slot].exchange(open ? std::uint8_t{0} : std::uint8_t{1}, std::memory_order_acq_rel);
  if (!open && was == 0u && bound(slot)) {
    const Command c{Cmd::Release, static_cast<std::uint8_t>(slot), 0, 0, 0, 0, 0.0f};
    if (!ring_.push(c)) {
      ++stats_.dropped;  // (the gate is closed: nothing new starts; a note that was on goes on until its own end)
    }
  }
}

bool ModuleVoices::set_param(unsigned slot, unsigned id, float value) noexcept {
  if (!bound(slot) || id > 0xFFFFu) {
    return false;
  }
  const Command c{Cmd::SetParam, static_cast<std::uint8_t>(slot), 0, 0, static_cast<std::uint16_t>(id), 0, value};
  if (!ring_.push(c)) {
    ++stats_.dropped;
    return false;
  }
  return true;
}

bool ModuleVoices::audition(unsigned slot, unsigned track, unsigned note, unsigned velocity, bool on) noexcept {
  if (!routed(slot) || track >= kModuleTracks || note > 127u) {
    return false;
  }
  const Command c{on ? Cmd::NoteOn : Cmd::NoteOff, static_cast<std::uint8_t>(slot), static_cast<std::uint8_t>(track),
                  static_cast<std::uint8_t>(velocity > 127u ? 127u : (velocity == 0u ? 1u : velocity)), static_cast<std::uint16_t>(note), 0, 0.0f};
  if (!ring_.push(c)) {
    ++stats_.dropped;
    return false;
  }
  return true;
}

unsigned ModuleVoices::instances() const noexcept {
  unsigned n = 0;
  for (unsigned i = 0; i < kMaxModuleInstances; ++i) {
    n += in_[i].mod != nullptr ? 1u : 0u;
  }
  return n;
}

bool ModuleVoices::info(unsigned i, Info& out) const noexcept {
  if (i >= kMaxModuleInstances || in_[i].mod == nullptr) {
    return false;
  }
  out.slot = in_[i].slot;
  out.mod = in_[i].mod;
  out.held = in_[i].held != 0u;
  out.active = in_[i].active != 0u;
  out.track = in_[i].track == kNone ? kNoTrack : in_[i].track;
  out.cost = in_[i].gov.cost.load(std::memory_order_relaxed);
  out.muted = in_[i].gov.muted.load(std::memory_order_relaxed) != 0u;
  out.state = in_[i].state;
  return true;
}

bool ModuleVoices::perf(unsigned i, ModulePerf& out) const noexcept {
  if (i >= kMaxModuleInstances || in_[i].mod == nullptr) {
    return false;
  }
  if (perf_reset_.load(std::memory_order_acquire) != perf_seen_.load(std::memory_order_relaxed)) {
    out = ModulePerf{};  // a reset the render has not served yet: what was measured before it no longer counts
    return true;
  }
  return in_[i].perf.read(out);
}

// ---- render context ---------------------------------------------------------------------------------------------------------------

void ModuleVoices::apply(const Command& c) noexcept {
  const std::uint8_t idx = c.slot < kModuleSlots ? slot_[c.slot].load(std::memory_order_acquire) : kNone;
  if (idx == kNone || in_[idx].ready.load(std::memory_order_acquire) == 0u) {
    return;
  }
  Instance& in = in_[idx];
  switch (c.kind) {
    case Cmd::SetParam: {
      const std::uint16_t prev = enter(in);
      in.mod->set_param(in.state, c.a, c.value);
      leave(prev);
      break;
    }
    case Cmd::NoteOn:
      if (gate_[c.slot].load(std::memory_order_acquire) != 0u) {
        break;  // OS-INSTUI S5: queued before the gate closed
      }
      audition_note(idx, c);  // a played key: over a held note it is legato (and, OS-GOV, a note like the others for the governor)
      break;
    case Cmd::NoteOff:
      note_off(c.track);
      break;
    case Cmd::Release:  // OS-INSTUI S5: the slot's instrument is gone: its notes end and its tracks are free
      for (unsigned t = 0; t < kModuleTracks; ++t) {
        if (owner_[t] == idx) {
          release_track(t);
        }
      }
      break;
  }
}

void ModuleVoices::begin_block() noexcept {
  blocks_.store(blocks_.load(std::memory_order_relaxed) + 1u, std::memory_order_release);
  touched_ = 0;
  seg_pos_ = 0;
  prof_ = perf_on_.load(std::memory_order_relaxed) != 0u;
  gov_.begin_block();
  Command c;
  for (std::uint32_t n = 0; n < kMaxCommandsPerBlock && ring_.pop(c); ++n) {
    apply(c);
  }
}

void ModuleVoices::render(std::uint32_t begin, std::uint32_t end) noexcept {
  const std::uint32_t n = end - begin;
  if (n == 0u || end > kBlock) {
    return;
  }
  seg_pos_ = static_cast<std::uint8_t>(end);  // (OS-MODPITCH: where the events that follow this stretch find the block)
  for (unsigned i = 0; i < kMaxModuleInstances; ++i) {
    Instance& in = in_[i];
    if (in.active == 0u || in.track == kNone || in.ready.load(std::memory_order_acquire) == 0u) {
      continue;
    }
    std::int16_t* block = block_[i];
    const bool first = ((touched_ >> i) & 1u) == 0u;
    const bool prof = prof_;  // OS-303PERF: `diag cpu reset` turned the split on (latched at the block start: a block is measured whole or not at all); until then these stamps are not read
    std::uint32_t tz = 0;
    if (first) {
      tz = prof ? cycles_() : 0u;  // (the zero-fill is the output side's too)
      zero_block(block);           // a note that starts mid-block is exactly silent before its sample
      touched_ |= 1u << i;
    }
    const std::uint32_t t0 = cycles_();  // OS-GOV: the instance's cost, the module's call and the conversion. OS-303PERF: five reads a segment (t0..t4) split it: enter, render alone, leave, conversion
    if (in.ramp_on != 0u) {
      ramp_block(in, begin, first);  // OS-MODPITCH: a Slide or Glide in progress: this block's stretch (inside the governor's window, outside the render() the perf split measures)
    }
    const std::uint16_t prev = enter(in);
    const std::uint32_t t1 = prof ? cycles_() : 0u;
    const std::uint32_t more = in.shift == 0u ? in.mod->render(in.state, nullptr, scratch_, n) : render_rated(in, n);  // (OS-MODRATE: a rated instance's call is in flash)
    const std::uint32_t t2 = prof ? cycles_() : 0u;
    leave(prev);
    const std::uint32_t t3 = prof ? cycles_() : 0u;
    stats_.saturated += to_q15(block + begin, scratch_, n);
    ++stats_.rendered;
    const std::uint32_t t4 = cycles_();
    in.gov.acc_cycles += t4 - t0;
    in.gov.acc_frames += n;
    if (prof) {
      in.acc_render += t2 - t1;
      in.acc_out += t4 - t3;
      if (first) {
        in.acc_zero = t0 - tz;  // (the first segment of the block: one a block)
      }
    }
    if (more == 0u) {
      in.active = 0;
    }
  }
}

// OS-MODRATE. `ph` counts the copies of `hold` already out (0: the next frame is a new module sample), so `ph` after `n` frames is (ph + n) mod 2^shift whatever the segmentation. The module renders at its
// own (reduced) rate, so a segment of n frames asks it for the samples that start inside it, ceil((n - lead) / 2^shift), and a sample cut by the end of the segment is held on into the next one.
std::uint32_t ModuleVoices::render_rated(Instance& in, std::uint32_t n) noexcept {
  const unsigned shift = in.shift;
  const std::uint32_t mask = (1u << shift) - 1u;
  float* const s = scratch_;
  std::uint32_t lead = in.ph != 0u ? (mask + 1u) - in.ph : 0u;  // frames that finish the sample held from before
  lead = lead > n ? n : lead;
  const std::uint32_t k = (n - lead + mask) >> shift;
  std::uint32_t more = 1u;
  if (k != 0u) {
    more = in.mod->render(in.state, nullptr, s, k);
  }
  const float held = in.hold;
  const float last = k != 0u ? s[k - 1u] : held;
  for (std::uint32_t o = n; o > lead;) {  // backwards: the sample read, (o - lead) >> shift, is never past o
    --o;
    s[o] = s[(o - lead) >> shift];
  }
  for (std::uint32_t o = 0; o < lead; ++o) {
    s[o] = held;
  }
  in.ph = static_cast<std::uint8_t>((in.ph + n) & mask);
  in.hold = last;
  return more;
}

void ModuleVoices::end_block() noexcept {
  if (touched_ != 0u) {
    account_block();
  }
}

void ModuleVoices::start_note(unsigned i, unsigned track, unsigned note, unsigned velocity, bool overlap) noexcept {
  Instance& in = in_[i];
  // OS-MODPITCH: a track that holds a Slide or Glide FX makes the note of a module with `pitch` a ramping, legato one (or a tie). The glide's memory is the last module note of the track.
  unsigned extra = 0;
  const unsigned prev1 = tfx_[track].last1;
  tfx_[track].last1 = static_cast<std::uint8_t>(note + 1u);
  if (in.pitch != nullptr && (((slide_on_ | glide_on_) >> track) & 1u) != 0u) {
    extra = ramp_plan(in, track, note, prev1);
    if (extra == kTie) {
      return;
    }
  }
  // Legato is a live key (or the audition) played over a held LIVE key: both sides are played by hand. `held` alone says nothing: a sequencer's note is never released by a note-off of its own, and a
  // pad struck over it is a new note on a voice that still rings (OS-MODSILENT review: sequencer then pad gave N L, a slide with no envelope and no sound from the pad). The one exception is a note
  // that slides over the note that sounds (extra).
  const bool legato = (overlap && in.held != 0u && in.held_overlap != 0u) || (extra & O3M_NOTE_LEGATO) != 0u;
  if (in.held != 0u && !legato && in.ready.load(std::memory_order_acquire) != 0u) {
    // A new note (OS-MODSILENT): the held note ends first, so that the module retriggers (the TB-303 took a held note as a slide and never started the envelope again).
    const std::uint16_t before = enter(in);
    in.mod->note_off(in.state, in.last_note);
    leave(before);
    in.held = 0;
    in.held_overlap = 0;
  }
  if (in.track != kNone && in.track != track) {
    owner_[in.track] = kNone;  // another track played this instrument: it loses it
  }
  const std::uint8_t other = owner_[track];
  if (other != kNone && other != i) {
    Instance& o = in_[other];
    if (o.held != 0u && o.ready.load(std::memory_order_acquire) != 0u) {
      const std::uint16_t prev = enter(o);
      o.mod->note_off(o.state, o.last_note);
      leave(prev);
    }
    o.held = 0;
    o.held_overlap = 0;
    o.active = 0;
    o.track = kNone;
  }
  owner_[track] = static_cast<std::uint8_t>(i);
  in.track = static_cast<std::uint8_t>(track);
  in.ph = 0;  // OS-MODRATE: a note's first frame is a new module sample (a legato note too: the held one is the old note's)
  const std::uint16_t prev = enter(in);
  in.mod->note_on(in.state, note, velocity, (legato ? O3M_NOTE_LEGATO : 0u) | (extra & O3M_NOTE_RAMP));
  leave(prev);
  in.held = 1;
  in.held_overlap = overlap ? 1 : 0;  // how this note started: only a live key may be slid into
  in.active = 1;
  in.last_note = static_cast<std::uint16_t>(note);
  in.ramp_on = 0;  // (a note puts the module's offset back to 0)
  if (extra != 0u) {
    ramp_start(in);
  }
  ++stats_.note_ons;
}

bool ModuleVoices::note_on(unsigned track, unsigned note, unsigned slot, unsigned velocity, bool overlap) noexcept {
  if (slot >= kModuleSlots || track >= kModuleTracks) {
    return false;
  }
  const std::uint8_t idx = slot_[slot].load(std::memory_order_acquire);
  if (idx == kNone || in_[idx].ready.load(std::memory_order_acquire) == 0u || gate_[slot].load(std::memory_order_acquire) != 0u) {  // (OS-INSTUI S5: a gated slot starts nothing)
    return false;
  }
  if (governed_ && !admit(idx, track)) {  // OS-GOV: a dropped note or a muted instance starts nothing (a retrigger of a sounding instrument is always admitted)
    return false;
  }
  const unsigned v = velocity == kVelocityDefault ? kDefaultVelocity : (velocity == 0u ? 1u : (velocity > 127u ? 127u : velocity));
  start_note(idx, track, note > 127u ? 127u : note, v, overlap);
  return true;
}

void ModuleVoices::note_off(unsigned track) noexcept {
  if (track >= kModuleTracks) {
    return;
  }
  const std::uint8_t i = owner_[track];
  if (i == kNone) {
    return;
  }
  Instance& in = in_[i];
  if (in.held != 0u && in.track == track && in.ready.load(std::memory_order_acquire) != 0u) {
    const std::uint16_t prev = enter(in);
    in.mod->note_off(in.state, in.last_note);
    leave(prev);
    in.held = 0;
    in.held_overlap = 0;
  }
}

void ModuleVoices::release_track(unsigned track) noexcept {
  if (track >= kModuleTracks) {
    return;
  }
  note_off(track);
  const std::uint8_t i = owner_[track];
  if (i != kNone && in_[i].track == track) {
    in_[i].track = kNone;
  }
  owner_[track] = kNone;
}

std::uint16_t ModuleVoices::slot_tracks(unsigned slot) const noexcept {
  std::uint16_t mask = 0;
  for (unsigned t = 0; t < kModuleTracks; ++t) {
    const std::uint8_t i = owner_[t];
    if (i != kNone && in_[i].track == t && in_[i].slot == slot && (in_[i].held | in_[i].active) != 0u) {
      mask = static_cast<std::uint16_t>(mask | (1u << t));
    }
  }
  return mask;
}

void ModuleVoices::release_notes() noexcept {
  for (unsigned t = 0; t < kModuleTracks; ++t) {
    note_off(t);
  }
}

// ---- OS-MODPITCH (render context, in flash: per FX event, per ramping note, once a block per ramp in progress) ----------------------------------------------------------

// The module's pitch() inside the crash context, with the numbers the ABI promises (finite by construction; held to +-128 here too).
void ModuleVoices::pitch_call(Instance& in, float semitones, float per_frame) noexcept {
  constexpr float kMax = 128.0f;
  semitones = semitones != semitones ? 0.0f : semitones;  // (NaN is no pitch: the ABI promises finite numbers)
  per_frame = per_frame != per_frame ? 0.0f : per_frame * static_cast<float>(1u << in.shift);  // (OS-MODRATE: the slope is per module frame, and a module frame spans 2^shift output frames)
  semitones = semitones > kMax ? kMax : (semitones < -kMax ? -kMax : semitones);
  per_frame = per_frame > kMax ? kMax : (per_frame < -kMax ? -kMax : per_frame);
  const std::uint16_t prev = enter(in);
  in.pitch(in.state, semitones, per_frame);
  leave(prev);
}

// One stretch of `ahead` samples (to the end of the block): the offset now and the slope that takes the module to where the walk puts the ramp (the sampler's evaluation: the same ramp_advance), or,
// for a ramp that has landed, the target and no slope.
void ModuleVoices::ramp_call(Instance& in, std::uint32_t ahead) noexcept {
  constexpr float kQ16 = 1.0f / 65536.0f;
  in.ramp_blk = static_cast<std::uint16_t>(blocks_.load(std::memory_order_relaxed));
  if (in.ramp_on == kRampSettle) {
    pitch_call(in, static_cast<float>(in.ramp.tgt) * kQ16, 0.0f);
    in.ramp_on = in.ramp.tgt == 0 ? std::uint8_t{0} : kRampHold;
    return;
  }
  if (ahead == 0u) {  // a note that starts once the block is rendered (seg_pos_ at its end): no stretch to walk, the offset as it is (the next block's call walks on); never a division by zero samples
    pitch_call(in, static_cast<float>(in.ramp.q16) * kQ16, 0.0f);
    in.ramp_on = kRampWalk;
    return;
  }
  const std::int32_t from = in.ramp.q16;
  const bool more = ramp_advance(in.ramp, ahead);
  pitch_call(in, static_cast<float>(from) * kQ16, static_cast<float>(in.ramp.q16 - from) / static_cast<float>(ahead * 65536u));
  in.ramp_on = more ? kRampWalk : kRampSettle;
}

void ModuleVoices::ramp_start(Instance& in) noexcept {
  in.ramp_on = kRampWalk;
  ramp_call(in, kBlock - seg_pos_);
}

void ModuleVoices::ramp_block(Instance& in, std::uint32_t begin, bool first) noexcept {
  if (!first || (in.ramp_on & kRampDue) == 0u) {
    return;  // (one call a block: the stretch that begins the block; a held offset needs none)
  }
  if (in.held == 0u) {
    in.ramp_on = 0;  // the note ended: the module sits at the offset it had; nothing is held, nothing is called
    return;
  }
  if (in.ramp_blk != static_cast<std::uint16_t>(blocks_.load(std::memory_order_relaxed))) {
    ramp_call(in, kBlock - begin);
  }
}

void ModuleVoices::ramp_snap(Instance& in) noexcept {
  if (in.ramp_on == 0u) {
    return;
  }
  const bool held = in.held != 0u;
  in.ramp = PitchRamp{};
  in.ramp_on = 0;
  if (held) {
    pitch_call(in, 0.0f, 0.0f);
  }
}

// What the track's FX make of a pattern note (see the declaration). A Slide FX wins over a Glide. The ramp is left in in.ramp.
unsigned ModuleVoices::ramp_plan(Instance& in, unsigned track, unsigned note, unsigned prev1) noexcept {
  const bool slide = ((slide_on_ >> track) & 1u) != 0u;
  PitchRamp r;
  if (slide) {
    if (tfx_[track].slide == 0) {
      return 0;  // a Slide of nothing is no slide
    }
    r.tgt = slide_target_q16(tfx_[track].slide);
    r.left = slide_step_samples(tick_rate_q24(tempo_x100_ != 0u ? tempo_x100_ : 12000u));
  } else {
    const std::uint32_t ms = glide_fx_ms(tfx_[track].glide);
    if (ms == 0u || prev1 == 0u) {
      return 0;  // no glide, or nothing to glide from (the track's first module note)
    }
    r.q16 = (static_cast<std::int32_t>(prev1) - 1 - static_cast<std::int32_t>(note)) * 65536;
    r.left = glide_samples(ms);
  }
  const bool here = in.held != 0u && in.track == track && in.ready.load(std::memory_order_relaxed) != 0u;  // the note sounds on this track: this one slides over it
  if (here && in.last_note == note) {
    return kTie;
  }
  if (!slide && prev1 - 1u == note) {
    return 0;  // a repeat of the same note after a release: the glide has nowhere to go (the sampler's rule too)
  }
  in.ramp = r;
  return O3M_NOTE_RAMP | (here ? O3M_NOTE_LEGATO : 0u);
}

// A ModSet / ModEnd of the Slide or Glide group, or an FxReset (the transport starts). The Glide is read at the next note; the Slide moves the module that sounds on the track from where it is to the
// distance the FX names from the note (a row without a note), and its end puts the note's own pitch back with one call.
void ModuleVoices::fx_event(const SeqEvent& e) noexcept {
  if (e.kind == EventKind::FxReset) {
    slide_on_ = 0;
    glide_on_ = 0;
    for (Instance& in : in_) {
      if (in.mod != nullptr && in.ready.load(std::memory_order_relaxed) != 0u) {
        ramp_snap(in);
      }
    }
    return;
  }
  const unsigned track = e.track;
  if (track >= kModuleTracks || (e.flags != kModGroupSlide && e.flags != kModGroupGlide)) {
    return;
  }
  const bool set = e.kind == EventKind::ModSet;
  const std::uint16_t bit = static_cast<std::uint16_t>(1u << track);
  if (e.flags == kModGroupGlide) {
    if (set && e.a == kFxGlide) {
      tfx_[track].glide = static_cast<std::uint8_t>(e.b > 100u ? 100u : e.b);
      glide_on_ = static_cast<std::uint16_t>(glide_on_ | bit);
    } else {
      glide_on_ = static_cast<std::uint16_t>(glide_on_ & ~bit);
    }
    return;
  }
  const std::uint8_t idx = owner_[track];
  Instance* const in = idx != kNone && in_[idx].track == track && in_[idx].held != 0u && in_[idx].pitch != nullptr && in_[idx].ready.load(std::memory_order_relaxed) != 0u ? &in_[idx] : nullptr;
  if (set && (e.a == kFxSlideUp || e.a == kFxSlideDown)) {
    const int sixteenths = e.a == kFxSlideDown ? -static_cast<int>(e.b) : static_cast<int>(e.b);
    const int d = sixteenths < -255 ? -255 : (sixteenths > 255 ? 255 : sixteenths);
    tfx_[track].slide = static_cast<std::int16_t>(d);
    slide_on_ = static_cast<std::uint16_t>(slide_on_ | bit);
    if (in != nullptr && (d != 0 || in->ramp_on != 0u)) {  // (a Slide of nothing over a note that is where it was: no ramp to make)
      in->ramp.q16 = in->ramp_on == 0u ? 0 : in->ramp.q16;  // no ramp: the module is at the note's own pitch, whatever an earlier ramp left in the struct
      in->ramp.tgt = slide_target_q16(d);
      in->ramp.left = slide_step_samples(tick_rate_q24(tempo_x100_ != 0u ? tempo_x100_ : 12000u));
      ramp_start(*in);
    }
    return;
  }
  slide_on_ = static_cast<std::uint16_t>(slide_on_ & ~bit);
  if (in != nullptr) {
    ramp_snap(*in);
  }
}

// ---- OS-GOV (render context, in flash: once a block, once a mute) ---------------------------------------------------------------------

void ModuleVoices::audition_note(unsigned i, const Command& c) noexcept {
  if (!governed_ || admit(i, c.track)) {
    start_note(i, c.track, c.a, c.velocity, true);
  }
}

// May a note start on instance i, for `track`? A refused note also releases what the track was playing: a track whose note was replaced does not go on sounding the old one (a sample note on the track
// does the same, Engine::dispatch).
bool ModuleVoices::admit(unsigned i, unsigned track) noexcept {
  Instance& in = in_[i];
  const std::uint32_t now = blocks_.load(std::memory_order_relaxed);
  // A voice is an instance that is RENDERING (`active`): a module whose amplitude has died away reports itself idle while its gate may still be held (a tracker sends no note-off), and costs nothing;
  // a note for it adds a voice, and it is no load for the others.
  std::uint32_t sounding = 0;  // what the OTHER voices that sound cost (the ones added in this block are in the governor's `pending`)
  for (unsigned k = 0; k < kMaxModuleInstances; ++k) {
    const Instance& o = in_[k];
    if (k != i && o.mod != nullptr && o.ready.load(std::memory_order_relaxed) != 0u && o.active != 0u) {
      sounding += gov::Governor::sounding_cost(o.gov, now);
    }
  }
  const gov::Verdict verdict = gov_.admit(in.gov, in.slot, in.active != 0u, sounding, now);
  if (verdict == gov::Verdict::Drop || verdict == gov::Verdict::Muted) {
    release_track(track);
    return false;
  }
  return true;
}

void ModuleVoices::block_done(const std::uint32_t (&stage_cycles)[hal::audio::kStageCount]) noexcept {
  std::uint32_t total = 0;
  for (unsigned k = hal::audio::kStageInput; k <= hal::audio::kStageTaps; ++k) {
    total += stage_cycles[k];
  }
  gov_.observe_block(total);
}

void ModuleVoices::account_block() noexcept {
  if (!governed_) {
    return;  // no counter: the instances were not measured
  }
  const std::uint32_t now = blocks_.load(std::memory_order_relaxed);
  const std::uint32_t reset = perf_reset_.load(std::memory_order_acquire);
  if (reset != perf_seen_.load(std::memory_order_relaxed)) {  // OS-303PERF: `diag cpu reset` asked: every instance starts its measure again (this block's figures, below, are the first)
    for (unsigned i = 0; i < kMaxModuleInstances; ++i) {
      in_[i].perf.clear();
    }
    perf_seen_.store(reset, std::memory_order_release);
  }
  for (unsigned i = 0; i < kMaxModuleInstances; ++i) {
    if (((touched_ >> i) & 1u) == 0u) {
      continue;
    }
    if (prof_) {
      profile(in_[i]);
    }
    if (gov_.account(in_[i].gov, in_[i].slot, now) == gov::Governor::Account::Muted) {
      silence(i);  // before the engine takes the tracks' feeds: this block's audio of the instance is not heard either
    }
  }
}

// A block of the instance in which it rendered all kBlock frames (however many segments) is a sample; one that began or ended mid-way is not (a note starting at frame 40 costs 24 frames, which
// would be a false minimum). The governor's account() takes acc_cycles and acc_frames right after this and clears them.
void ModuleVoices::profile(Instance& in) noexcept {
  if (in.gov.acc_frames == kBlock) {
    in.perf.add(in.gov.acc_cycles + in.acc_zero, in.acc_render, in.acc_out + in.acc_zero);
  }
  in.acc_render = in.acc_out = in.acc_zero = 0;
}

// ---- OS-303PERF: the perf cell (a seqlock: add() has one writer, the render; clear() has two, the render and bind(), see PerfCell) ------------------------------------

void ModuleVoices::PerfCell::clear() noexcept {
  const std::uint32_t s = seq_.load(std::memory_order_relaxed);
  seq_.store(s + 1u, kPerfData);  // odd: a reader that looks now looks again
  perf_fence_release();
  for (unsigned k = 0; k < kWords; ++k) {
    w_[k].store(0u, kPerfData);
  }
  w_[kLoopMin].store(0xFFFFFFFFu, kPerfData);
  w_[kRenderMin].store(0xFFFFFFFFu, kPerfData);
  w_[kOutMin].store(0xFFFFFFFFu, kPerfData);
  seq_.store(s + 2u, std::memory_order_release);
}

// min, max and the 64-bit sum of one bucket, in the words from `first` (clear() put the minima at the top, so the first sample is both).
void ModuleVoices::PerfCell::bucket(unsigned first, std::uint32_t v) noexcept {
  if (v < w_[first].load(std::memory_order_relaxed)) {
    w_[first].store(v, kPerfData);
  }
  if (v > w_[first + 1u].load(std::memory_order_relaxed)) {
    w_[first + 1u].store(v, kPerfData);
  }
  const std::uint32_t lo = w_[first + 2u].load(std::memory_order_relaxed) + v;
  w_[first + 2u].store(lo, kPerfData);
  if (lo < v) {  // the carry of the 64-bit sum
    w_[first + 3u].store(w_[first + 3u].load(std::memory_order_relaxed) + 1u, kPerfData);
  }
}

void ModuleVoices::PerfCell::add(std::uint32_t loop, std::uint32_t render, std::uint32_t out) noexcept {
  const std::uint32_t s = seq_.load(std::memory_order_relaxed);
  seq_.store(s + 1u, kPerfData);
  perf_fence_release();
  bucket(kLoopMin, loop);
  bucket(kRenderMin, render);
  bucket(kOutMin, out);
  w_[kBlocks].store(w_[kBlocks].load(std::memory_order_relaxed) + 1u, kPerfData);  // (wraps after 2^32 blocks, 72 days of sounding: the sums are 64-bit, the count is not)
  seq_.store(s + 2u, std::memory_order_release);
}

bool ModuleVoices::PerfCell::read(ModulePerf& out) const noexcept {
  for (unsigned tries = 0; tries < 4u; ++tries) {
    const std::uint32_t a = seq_.load(std::memory_order_acquire);
    if ((a & 1u) != 0u) {
      continue;
    }
    std::uint32_t t[kWords];
    for (unsigned k = 0; k < kWords; ++k) {
      t[k] = w_[k].load(kPerfData);
    }
    perf_fence_acquire();
    if (seq_.load(kPerfData) != a) {
      continue;
    }
    if (t[kBlocks] == 0u) {
      out = ModulePerf{};  // (the minima still hold their start value)
      return true;
    }
    const auto sum = [&](unsigned lo) { return static_cast<std::uint64_t>(t[lo + 1u]) << 32 | t[lo]; };
    out.blocks = t[kBlocks];
    out.loop_min = t[kLoopMin];
    out.loop_max = t[kLoopMax];
    out.loop_sum = sum(kLoopLo);
    out.render_min = t[kRenderMin];
    out.render_max = t[kRenderMax];
    out.render_sum = sum(kRenderLo);
    out.out_min = t[kOutMin];
    out.out_max = t[kOutMax];
    out.out_sum = sum(kOutLo);
    return true;
  }
  return false;
}

// The safety net's mute: the instance's note ends (note_off, its release is not rendered) and it stops being rendered. The block it was rendered in is not cut: it is faded out over the block (a hard cut
// is a click), and the instance keeps its track for that block so that the feed still carries it; from the next block on it is not rendered, so the feed is null. A later note clears the mute
// (Governor::admit).
void ModuleVoices::silence(unsigned i) noexcept {
  Instance& in = in_[i];
  if (in.held != 0u && in.ready.load(std::memory_order_acquire) != 0u) {
    const std::uint16_t prev = enter(in);
    in.mod->note_off(in.state, in.last_note);
    leave(prev);
  }
  in.held = 0;
  in.held_overlap = 0;
  in.active = 0;
  std::int16_t* block = block_[i];
  for (std::uint32_t k = 0; k < kBlock; ++k) {
    block[k] = static_cast<std::int16_t>(static_cast<std::int32_t>(block[k]) * static_cast<std::int32_t>(kBlock - k) / static_cast<std::int32_t>(kBlock));
  }
}

}  // namespace o3os::audio
