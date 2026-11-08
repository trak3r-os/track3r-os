// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/ptfx_job.h>

#include <algorithm>
#include <cstring>

namespace o3os::app {

namespace {

constexpr std::uint32_t kZeroChunkBytes = 64u * 1024u;  // 3.5: the scratch is zeroed 64 KiB a step
constexpr std::uint32_t kCopyChunkFrames = 16384;       // a copy step moves 32 KiB (as the other edit jobs' chunks, well inside a slice)
constexpr std::uint32_t kMaxResultFrames = 0x3FFFFFFFu; // what SampleBank::edit_begin accepts (it refuses a larger one itself)

// The default CallFn: the call gate, with the plugin's stack and image. On the target every address fits 32 bits; the host stand-ins of the tests bring their own CallFn.
hal::code_gate::GateResult gate_call(const PtfxJob::Plugin& p, std::uintptr_t entry, std::uintptr_t r0, std::uintptr_t r1, std::uintptr_t r2, std::uintptr_t r3, float s0,
                                     std::int32_t* ret) noexcept {
  return hal::code_gate::call(reinterpret_cast<hal::code_gate::Entry>(entry), static_cast<std::uint32_t>(r0), static_cast<std::uint32_t>(r1), static_cast<std::uint32_t>(r2),
                              static_cast<std::uint32_t>(r3), s0, kPtfxCallTimeoutMs, p.stack_top, p.stack_bottom, p.image_base, p.image_bytes, p.id, ret);
}

// S2c's rule: an id the quarantine holds as Why::Crash comes from a crash record, which may name a .tmod: it counts only when a listed plugin has it (the catalogue is the judge). A
// timeout, fault or overflow was put there by a run of this very plugin and always counts.
bool quarantined(const PtfxJob::Plugin& p) noexcept {
  const module::PtfxQuarantine* q = p.quarantine;
  if (q == nullptr || !q->contains(p.id)) {
    return false;
  }
  if (q->why_of(p.id) != module::PtfxQuarantine::Why::Crash || p.catalog == nullptr) {
    return true;
  }
  for (unsigned i = 0; i < p.catalog->count(); ++i) {
    if (p.catalog->entry(i).id == p.id) {
      return true;
    }
  }
  return false;
}

std::int16_t sat16(std::int32_t v) noexcept { return static_cast<std::int16_t>(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }

constexpr std::uint32_t kConvertChunkFrames = 8192;  // Convert's step: about 70k cycles at the most, a slice well inside the budget

static_assert(kPtfxBlockFrames * 2u * sizeof(std::int16_t) == 32u * 1024u, "the staging of the memory map (kPtfxStereoSize) holds L and R of one block each");

}  // namespace

const char* PtfxJob::begin(const Plugin& plugin, const Source& source) noexcept {
  const Plugin& p = plugin;
  const bool gen = p.info != nullptr && (p.info->flags & module::kPtfxFlagGenerator) != 0u;  // a generator needs no sample (an empty slot is fine)
  if (source.alloc == nullptr || (!gen && (source.pcm == nullptr || source.frames == 0u))) {
    return "NOTHING TO PROCESS";
  }
  if (p.info == nullptr || p.entries.init == 0u || p.entries.set_param == 0u || p.entries.process_block == 0u || p.host == nullptr || !p.host->valid() ||
      p.scratch == nullptr) {
    why_ = p.host == nullptr || !p.host->valid() ? "host" : "plugin";
    return "PLUGIN NOT READY";
  }
  const bool st = !gen && source.stereo;  // 3.6 / A12: a generator ignores the source, so a stereo slot is no matter to it (its result is mono)
  if (st && (source.side == nullptr || p.stage == nullptr)) {
    why_ = "stereo";
    return "PLUGIN NOT READY";  // the S block and the staging are what a stereo run converts through
  }
  if (quarantined(p)) {
    return "PLUGIN QUARANTINED";
  }
  if (p.info->scratch_size > p.scratch_cap || p.info->scratch_size > module::kPtfxMaxScratch) {
    why_ = "scratch";
    return "PLUGIN NOT READY";  // init must get the whole scratch it asked for (2.2), never a smaller one
  }
  plugin_ = p;
  src_ = source;
  gen_ = gen;
  if (gen) {  // the source is not read: no body, no copy; Size gives the buffer kPtfxGeneratorFrames
    src_.pcm = nullptr;
    src_.side = nullptr;
    src_.stereo = false;
    src_.frames = src_.a = src_.b = 0;
  }
  if (src_.a > src_.b) {
    std::swap(src_.a, src_.b);
  }
  src_.b = std::min(src_.b, src_.frames);
  src_.a = std::min(src_.a, src_.b);
  if (src_.a == src_.b) {  // no range: the whole sample
    src_.a = 0;
    src_.b = src_.frames;
  }
  if (!st) {
    mode_ = Mode::Mono;
  } else if ((p.info->flags & module::kPtfxFlagConvertToMono) != 0u) {
    mode_ = Mode::ToMono;  // 3.6: channels = 2 over the whole sample, whatever the selection; the result is L, mono
    src_.a = 0;
    src_.b = src_.frames;
  } else {
    mode_ = (p.info->flags & module::kPtfxFlagStereo) != 0u ? Mode::Stereo : Mode::TwoPass;
  }
  pass_ = lane_ = 0;
  even_ = pass0_ = conv_ = 0;
  scratch_bytes_ = p.info->scratch_size;
  std::memcpy(name_, "PLUGIN ", 7);
  unsigned n = 7;
  for (const char* c = p.info->name; *c != '\0' && n < 19; ++c, ++n) {  // upper case, 12 characters of the display name
    name_[n] = (*c >= 'a' && *c <= 'z') ? static_cast<char>(*c - ('a' - 'A')) : *c;
  }
  name_[n] = '\0';
  detail_ = "";
  dst_ = nullptr;
  zeroed_ = pos_ = total_ = copied_ = result_ = target_ = 0;
  prepend_ = append_ = 0;
  param_ = 0;
  phase_ = Phase::ZeroScratch;
  return nullptr;
}

core::JobStatus PtfxJob::fail(const char* why) noexcept {
  detail_ = why;
  phase_ = Phase::Ended;
  return core::JobStatus::Failed;
}

// One bounded chunk of a copy phase: `count` frames from `from` to `to`, `copied_` of them done. True when the phase is complete.
bool PtfxJob::copy_step(const std::int16_t* from, std::int16_t* to, std::uint32_t count) noexcept {
  const std::uint32_t n = std::min(kCopyChunkFrames, count - copied_);
  std::memcpy(to + copied_, from + copied_, std::size_t{n} * sizeof(std::int16_t));
  copied_ += n;
  if (copied_ < count) {
    return false;
  }
  copied_ = 0;
  return true;
}

// copy_step of M (`from` frames into the source, `to` frames into the result), then, when the result is M and S, of S: true when both are complete. (One chunk a step; the S block of the result
// starts at even_.)
bool PtfxJob::copy_lanes(std::uint32_t from, std::uint32_t to, std::uint32_t count) noexcept {
  if (lane_ == 0u) {
    if (!copy_step(src_.pcm + from, dst_ + to, count)) {
      return false;
    }
    if (!stereo_result()) {
      return true;
    }
    lane_ = 1;
    return false;
  }
  if (!copy_step(src_.side + from, dst_ + even_ + to, count)) {
    return false;
  }
  lane_ = 0;
  return true;
}

// The input of one block as the plugin takes it: `n` frames of M and S turned into L = sat(M + S) (into stage[0..n)) and R = sat(M - S) (into stage[kPtfxBlockFrames..)), as asked.
void PtfxJob::stage_block(const std::int16_t* m, const std::int16_t* s, std::uint32_t n, bool left, bool right) noexcept {
  std::int16_t* const l = plugin_.stage;
  std::int16_t* const r = left ? l + kPtfxBlockFrames : l;  // (a pass of one channel uses the first half, whichever it is)
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::int32_t mi = m[i];
    const std::int32_t si = s[i];
    if (left) {
      l[i] = sat16(mi + si);
    }
    if (right) {
      r[i] = sat16(mi - si);
    }
  }
}

// One plugin call. False: the call did not come back clean, the job is already ended (and the plugin quarantined when it was the plugin's fault).
bool PtfxJob::call(std::uintptr_t entry, std::uintptr_t r0, std::uintptr_t r1, std::uintptr_t r2, std::uintptr_t r3, float s0, std::int32_t* ret) noexcept {
  const Plugin& p = plugin_;
  hal::code_gate::GateResult g = (p.call != nullptr ? p.call : gate_call)(p, entry, r0, r1, r2, r3, s0, ret);
  if (g == hal::code_gate::GateResult::Ok && p.guard != nullptr && !module::ptfx_check_guard(p.guard, p.guard_bytes)) {
    g = hal::code_gate::GateResult::StackOverflow;  // it ran into the guard and came back
  }
  if (g == hal::code_gate::GateResult::Ok) {
    return true;
  }
  p.host->disarm();
  using Why = module::PtfxQuarantine::Why;
  const char* text = "PLUGIN COULD NOT RUN";  // Unavailable: nothing ran
  if (g != hal::code_gate::GateResult::Unavailable) {
    Why why = Why::Timeout;
    text = "PLUGIN TIMED OUT: QUARANTINED";
    if (g == hal::code_gate::GateResult::Fault) {
      why = Why::Fault;
      text = "PLUGIN CRASHED: SAVE, THEN RESTART";
    } else if (g == hal::code_gate::GateResult::StackOverflow) {
      why = Why::StackOverflow;
      text = "PLUGIN STACK OVERFLOW: SAVE, THEN RESTART";
    }
    if (p.quarantine != nullptr) {
      (void)p.quarantine->quarantine(p.id, why);
    }
    if (p.catalog != nullptr) {
      (void)p.catalog->quarantine(p.id);
    }
  }
  (void)fail(text);
  return false;
}

core::JobStatus PtfxJob::step(std::uint32_t /*budget_us*/) noexcept {
  const Plugin& p = plugin_;
  const std::uint32_t a = src_.a;
  const std::uint32_t body = src_.b - src_.a;
  const std::uint32_t tail = src_.frames - src_.b;
  std::int32_t ret = 0;
  switch (phase_) {
    case Phase::ZeroScratch: {
      const std::uint32_t n = std::min(kZeroChunkBytes, scratch_bytes_ - zeroed_);
      std::memset(static_cast<std::uint8_t*>(p.scratch) + zeroed_, 0, n);  // (the scratch is the plugin's: zeroed before every init, 2.2)
      zeroed_ += n;
      phase_ = zeroed_ >= scratch_bytes_ ? Phase::Init : phase_;
      return core::JobStatus::Running;
    }
    case Phase::Init:
      if (p.guard != nullptr) {
        module::ptfx_paint_guard(p.guard, p.guard_bytes);
      }
      p.host->build(p.sample_rate);  // the table, the rand seed and the latch afresh: every run is repeatable (A8)
      p.host->arm();                 // A10: set_extra_samples counts from here to the end of the replay
      if (!call(p.entries.init, reinterpret_cast<std::uintptr_t>(p.scratch), scratch_bytes_, p.host_table, 0u, 0.0f, &ret)) {
        return core::JobStatus::Failed;
      }
      phase_ = Phase::Params;
      return core::JobStatus::Running;
    case Phase::Params: {
      const unsigned i = param_;
      const float v = i < p.info->num_params ? (p.values != nullptr ? p.values[i] : p.info->params[i].def) : 0.0f;  // all 12 indices, 0.0 past num_params
      if (!call(p.entries.set_param, i, 0u, 0u, 0u, v, &ret)) {
        return core::JobStatus::Failed;
      }
      if (++param_ >= kPtfxParamSlots) {
        p.host->disarm();
        phase_ = Phase::Size;
      }
      return core::JobStatus::Running;
    }
    case Phase::Size: {
      if (pass_ != 0u) {  // TwoPass, the R pass: the plugin, re-initialised, must ask for what it asked for on the L pass (the buffer is sized by it)
        if (p.host->latched_prepend() != prepend_ || p.host->latched_append() != append_) {
          return fail("STEREO: CHANNELS DIFFER");
        }
        phase_ = Phase::Process;
        return core::JobStatus::Running;
      }
      if (gen_) {  // the latch is not read; the result is 88,200 frames at most, trimmed to what Process produces
        dst_ = src_.alloc(src_.alloc_ctx, kPtfxGeneratorFrames, false);
        if (dst_ == nullptr) {
          return fail("NO ROOM FOR THE EDIT");
        }
        total_ = target_ = kPtfxGeneratorFrames;
        phase_ = Phase::Process;  // body is 0 and prepend 0: every block has `in` NULL
        return core::JobStatus::Running;
      }
      prepend_ = p.host->latched_prepend();
      append_ = p.host->latched_append();
      // The result is the whole sample plus what the plugin asked for before and after the body; each addend is checked against what is left below kMaxResultFrames (what the edit
      // pool can ever hold; it then refuses what it cannot fit), so the 32-bit sum cannot wrap: the buffer is exactly this long and no write below goes past it.
      const std::uint32_t f = src_.frames;
      if (f > kMaxResultFrames || prepend_ > kMaxResultFrames - f || append_ > kMaxResultFrames - f - prepend_) {
        return fail("NO ROOM FOR THE EDIT");
      }
      const std::uint32_t result = f + prepend_ + append_;
      // A stereo run takes an M and S buffer even for ToMono (the discarded R needs a place to land); its result is then committed as mono. Until Pack the S block stays at even_.
      dst_ = src_.alloc(src_.alloc_ctx, result, mode_ != Mode::Mono);
      if (dst_ == nullptr) {
        return fail("NO ROOM FOR THE EDIT");
      }
      even_ = (result + 1u) & ~1u;
      total_ = prepend_ + body + append_;
      target_ = result;
      copied_ = 0;
      phase_ = Phase::CopyBefore;
      return core::JobStatus::Running;
    }
    case Phase::CopyBefore:
      if (copy_lanes(0, 0, a)) {
        phase_ = Phase::Process;
      }
      return core::JobStatus::Running;
    case Phase::Process: {
      bool end = pos_ >= total_;
      if (!end) {
        // The block never crosses a boundary of prepend / body / append; `in` is the source at the body's offset, NULL in the other two regions.
        std::uint32_t left = total_ - pos_;
        const std::int16_t* in = nullptr;
        if (pos_ < prepend_) {
          left = prepend_ - pos_;
        } else if (pos_ < prepend_ + body) {
          left = prepend_ + body - pos_;
          in = src_.pcm + a + (pos_ - prepend_);
        }
        const std::uint32_t block = std::min(kPtfxBlockFrames, left);
        std::uint32_t channels = 1;
        in_arr_[0] = in_arr_[1] = nullptr;
        out_arr_[0] = dst_ + a + pos_;
        if (mode_ == Mode::Mono) {
          in_arr_[0] = in;
        } else {
          // Stereo: L and R both ways; the output goes into the M and S blocks as L and R for Convert. TwoPass: one channel a pass (the L pass into the M block, the R pass into the S block).
          // ToMono: both ways, R is discarded (it lands in the S block, which the mono commit drops).
          const bool two = mode_ != Mode::TwoPass;
          const bool lpass = pass_ == 0u;
          if (in != nullptr) {
            const std::uint32_t off = static_cast<std::uint32_t>(in - src_.pcm);
            stage_block(in, src_.side + off, block, two || lpass, two || !lpass);
            in_arr_[0] = plugin_.stage;
            in_arr_[1] = two ? plugin_.stage + kPtfxBlockFrames : nullptr;
          }
          if (two) {
            channels = 2;
            out_arr_[1] = dst_ + even_ + a + pos_;
          } else if (!lpass) {
            out_arr_[0] = dst_ + even_ + a + pos_;
          }
        }
        if (!call(p.entries.process_block, reinterpret_cast<std::uintptr_t>(in_arr_), reinterpret_cast<std::uintptr_t>(out_arr_), block, channels, 0.0f, &ret)) {
          return core::JobStatus::Failed;
        }
        if (ret > 0 && static_cast<std::uint32_t>(ret) > block) {
          return fail("PLUGIN ERROR");  // A11: it claims more than it was asked for
        }
        end = ret <= 0;  // A11: 0 or less ends the run, the frames produced so far are the result
        pos_ += end ? 0u : static_cast<std::uint32_t>(ret);
        end = end || pos_ >= total_;
      }
      if (end) {
        result_ = a + pos_ + tail;
        if (result_ == 0u) {
          return fail("NOTHING PRODUCED");
        }
        copied_ = 0;
        if (gen_) {  // nothing after the body to copy
          phase_ = Phase::Done;
          return core::JobStatus::Done;
        }
        if (mode_ == Mode::TwoPass) {
          if (pass_ == 0u) {  // the L pass is done: the plugin starts again (scratch, init, parameters) for R
            pass0_ = pos_;
            pass_ = 1;
            zeroed_ = pos_ = 0;
            param_ = 0;
            phase_ = Phase::ZeroScratch;
            return core::JobStatus::Running;
          }
          if (pos_ != pass0_) {
            return fail("STEREO: CHANNELS DIFFER");  // A13: both channels must come out the same length
          }
        }
        conv_ = 0;
        phase_ = (mode_ == Mode::Mono || mode_ == Mode::ToMono) ? Phase::CopyAfter : Phase::Convert;
      }
      return core::JobStatus::Running;
    }
    case Phase::Convert: {  // L and R in the M and S blocks of [a, a + pos_) back to M and S, in place (the importer's rule, see the header)
      const std::uint32_t n = std::min(kConvertChunkFrames, pos_ - conv_);
      std::int16_t* const m = dst_ + a + conv_;
      std::int16_t* const s = dst_ + even_ + a + conv_;
      for (std::uint32_t i = 0; i < n; ++i) {
        const std::int32_t l = m[i];
        const std::int32_t r = s[i];
        const std::int32_t mi = (l + r + 1) >> 1;
        m[i] = static_cast<std::int16_t>(mi);
        s[i] = sat16(l - mi);
      }
      conv_ += n;
      if (conv_ >= pos_) {
        phase_ = Phase::CopyAfter;
      }
      return core::JobStatus::Running;
    }
    case Phase::CopyAfter:
      if (copy_lanes(src_.b, a + pos_, tail)) {
        const bool pack = stereo_result() && ((result_ + 1u) & ~1u) != even_;
        phase_ = pack ? Phase::Pack : Phase::Done;
        return pack ? core::JobStatus::Running : core::JobStatus::Done;
      }
      return core::JobStatus::Running;
    case Phase::Pack: {  // the produced range was shorter than the room: the S block moves down to where the bank wants it (ascending chunks, so the overlap is safe)
      const std::uint32_t n = std::min(kCopyChunkFrames, result_ - copied_);
      std::memmove(dst_ + ((result_ + 1u) & ~1u) + copied_, dst_ + even_ + copied_, std::size_t{n} * sizeof(std::int16_t));
      copied_ += n;
      if (copied_ >= result_) {
        phase_ = Phase::Done;
        return core::JobStatus::Done;
      }
      return core::JobStatus::Running;
    }
    case Phase::Done:
      return core::JobStatus::Done;
    case Phase::Idle:
    case Phase::Ended:
      break;
  }
  return core::JobStatus::Failed;
}

unsigned PtfxJob::progress() const noexcept {
  switch (phase_) {
    case Phase::CopyBefore: return core::permille_of(copied_, target_);
    // (32-bit: every sum is at most twice the result, below 2^31; the two passes of TwoPass share the bar)
    case Phase::Process: return core::permille_of(src_.a + pos_ + pass_ * target_, mode_ == Mode::TwoPass ? 2u * target_ : target_);
    case Phase::Convert: return core::permille_of(src_.a + conv_, target_);
    case Phase::CopyAfter: return core::permille_of(src_.a + pos_ + copied_, target_);
    case Phase::Pack: return core::permille_of(copied_, result_);
    case Phase::Done: return core::kJobProgressMax;
    default: return 0;
  }
}

void PtfxJob::cancel() noexcept {
  if (phase_ == Phase::Params || phase_ == Phase::Init) {
    plugin_.host->disarm();
  }
  if (phase_ != Phase::Done) {
    phase_ = Phase::Ended;  // nothing is committed: the buffer is a free pool buffer again
  }
}

}  // namespace o3os::app
