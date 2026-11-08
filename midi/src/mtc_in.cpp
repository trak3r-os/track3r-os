// SPDX-License-Identifier: GPL-3.0-or-later
//
// MIDI Time Code in (midi/mtc_in.h): the chaser.
#include <o3os/midi/mtc_in.h>

namespace o3os::midi::mtc {

namespace {

constexpr std::uint32_t kRatioFrac = 21;  // the rate ratio is a Q21 number: a Q32 span below 2^41 times it stays below 2^63

core::SQ32 floor_div(core::SQ32 a, core::SQ32 b) noexcept {  // b > 0
  core::SQ32 q = a / b;
  if ((a % b) != 0 && a < 0) {
    --q;
  }
  return q;
}

// (a - b) as a signed distance on a circle of `m` positions, in [-m/2, m/2).
std::int32_t wrap_diff(std::uint32_t a, std::uint32_t b, std::uint32_t m) noexcept {
  const std::uint32_t d = (a + m - b % m) % m;
  return d >= m / 2u ? static_cast<std::int32_t>(d) - static_cast<std::int32_t>(m) : static_cast<std::int32_t>(d);
}

// The first sample a stamp falls in, on the timeline: the map's reference plus the stamp's distance from it.
std::uint64_t sample_of(const ClockMap& map, std::uint32_t cycles) noexcept {
  const std::int64_t rel = core::sq32_round(map.to_rel(cycles, map.ref_sample()));
  const std::int64_t s = static_cast<std::int64_t>(map.ref_sample()) + rel;
  return s < 0 ? 0u : static_cast<std::uint64_t>(s);
}

}  // namespace

void Chaser::reset() noexcept {
  to_idle();
  stats_ = Stats{};
  locate_ = Locate{};
  last_label_ = Time{};
}

void Chaser::drop_mapping() noexcept {
  known_ = false;
  asm_n_ = 0;
  ++epoch_;
}

void Chaser::to_idle() noexcept {
  state_ = State::Idle;
  known_ = false;
  asm_n_ = 0;
  have_prev_ = false;
  fwd_run_ = 0;
  rev_run_ = 0;
  reverse_ = false;
  refused_ = false;
  rate_bad_run_ = 0;
  rate_ok_run_ = 0;
  code_known_ = false;
  tick_base_ = 0;
  slip_hist_ = 0;
  refit_run_ = 0;
  ++epoch_;
}

// The first quarter frame of a reading: the loop starts again from it (the day's mapping is made later by the first whole cycle).
void Chaser::begin_reading(const ClockMap& map) noexcept {
  dll_.reset(map.ref_sample(), qf_time_real(1, 1));  // nominal: 25 fps (the fit overrides it after two quarter frames)
  state_ = State::Reading;
  tick_base_ = 0;
  slip_hist_ = 0;
  refit_run_ = 0;
  ++epoch_;
}

void Chaser::set_map(std::uint64_t tick, std::uint32_t q) noexcept {
  tick_ref_ = tick;
  q_ref_ = q;
}

std::uint32_t Chaser::q_of_tick(std::uint64_t tick) const noexcept {
  const std::int64_t qpd = static_cast<std::int64_t>(quarter_frames_per_day_of_code(code_));
  const std::int64_t d = static_cast<std::int64_t>(tick - tick_ref_);
  std::int64_t q = (static_cast<std::int64_t>(q_ref_) + d % qpd) % qpd;
  if (q < 0) {
    q += qpd;
  }
  return static_cast<std::uint32_t>(q);
}

std::int32_t Chaser::rate_ppm() const noexcept {
  if (!dll_.running()) {
    return 0;
  }
  const core::SQ32 nominal = static_cast<core::SQ32>(qf_time_real(1, code_));
  const core::SQ32 period = static_cast<core::SQ32>(dll_.period());
  if (period <= 0) {
    return 0;
  }
  return static_cast<std::int32_t>((nominal - period) * 1000000 / period);
}

Chaser::Notice Chaser::feed(const MtcEvent& e, const ClockMap& map) noexcept {
  return e.kind == MtcEvent::Kind::Qf ? on_qf(e, map) : on_full(e, map);
}

Chaser::Notice Chaser::poll(std::uint64_t now_sample) noexcept { return check_quiet(now_sample); }

// Silence: Freewheel after freewheel_qf periods (only a run has anything to freewheel), the stop after stop_qf. Called with the block's start and with the stamp of a message that comes late.
Chaser::Notice Chaser::check_quiet(std::uint64_t now_sample) noexcept {
  if (state_ == State::Idle || now_sample <= last_sample_) {
    return Notice::None;
  }
  const core::SQ32 quiet = static_cast<core::SQ32>(now_sample - last_sample_) * static_cast<core::SQ32>(core::kQ32One);
  const core::SQ32 period = static_cast<core::SQ32>(dll_.period());
  if (quiet > period * static_cast<core::SQ32>(cfg_.stop_qf)) {
    ++stats_.stops;
    to_idle();
    return Notice::Stopped;
  }
  if ((state_ == State::Armed || state_ == State::Locked) && quiet > period * static_cast<core::SQ32>(cfg_.freewheel_qf)) {
    resume_ = state_;
    state_ = State::Freewheel;
  }
  return Notice::None;
}

Chaser::Notice Chaser::on_qf(const MtcEvent& e, const ClockMap& map) noexcept {
  Notice n = Notice::None;
  const std::uint64_t at = sample_of(map, e.cycles);
  if (state_ != State::Idle) {
    n = check_quiet(at);  // a message that comes after the master was silent: the stop first, then this is the first quarter frame of a new reading
  }
  if (state_ == State::Idle) {
    begin_reading(map);
  }
  // The loop is fitting (a new reading, or one that lost its fit). With no mapping yet, a gap of two periods or more is a quarter frame lost: it would count for one tick and one point of
  // the fit, so start the fit again from this one. With a mapping the loop has no business fitting (it re-acquired: a long gap, a rate it could not follow): its ticks are no longer the
  // master's count, and a restart of the fit must not shift the mapping or wipe the error history of a run that is going well, so this is the loss of the master: the reading starts again.
  bool refit = false;
  if (dll_.has_phase() && !dll_.running()) {
    if (known_) {
      ++stats_.fit_losses;
      if (running_state() && n == Notice::None) {
        n = Notice::Stopped;
      }
      to_idle();
      begin_reading(map);
    } else if (refit_run_ < 3u && dll_.period() != 0u && static_cast<std::int32_t>(e.cycles - last_cycles_) > 0) {
      const core::Q32 per = dll_.period();
      const core::Q32 gap = core::q32_from_cycles(static_cast<std::uint32_t>(e.cycles - last_cycles_));
      const std::uint64_t steps = (2u * gap + per) / (2u * per);  // the periods the gap holds, rounded
      if (steps >= 2u) {
        tick_base_ += dll_.tick() + steps;
        dll_.reset(dll_.origin(), per);
        ++stats_.refits;
        refit = true;
      }
    }
  }
  refit_run_ = refit ? static_cast<std::uint8_t>(refit_run_ + 1u) : std::uint8_t{0};
  const ClockDll::Result r = dll_.on_clock(map.to_rel(e.cycles, dll_.origin()));
  if (r == ClockDll::Result::Ignored) {
    ++stats_.ignored;
    return n;
  }
  // A master the loop cannot keep in step with its pieces is not followed (SLIP in the header).
  const std::uint32_t piece = (e.data >> 4) & 7u;
  bool slip = false;
  if (known_ && have_prev_) {
    const std::uint32_t dt = static_cast<std::uint32_t>(eff_tick() - prev_tick_) & 7u;
    const bool reverse_step = dt == 1u && piece == ((prev_piece_ + 7u) & 7u);
    slip = !reverse_step && ((piece - prev_piece_) & 7u) != dt;
  }
  slip_hist_ = (slip_hist_ << 1) | (slip ? 1u : 0u);
  std::uint32_t slips = 0;
  for (std::uint64_t h = slip_hist_; h != 0u; h &= h - 1u) {
    ++slips;
  }
  if (slips >= cfg_.slip_qf) {
    ++stats_.slips;
    if (running_state() && n == Notice::None) {
      n = Notice::Stopped;
    }
    to_idle();
    begin_reading(map);
    (void)dll_.on_clock(map.to_rel(e.cycles, dll_.origin()));
  }
  ++stats_.qf;
  last_sample_ = at;
  last_cycles_ = e.cycles;
  if (state_ == State::Freewheel) {
    state_ = resume_;
  }

  const std::uint64_t tick = eff_tick();
  assemble(piece, e.data & 0x0Fu, tick, n);

  // The master's rate against ours, once the loop runs: refused after rate_hold_qf quarter frames outside the limits, accepted after as many inside.
  bool in_band = false;
  if (code_known_ && dll_.running()) {
    const std::int32_t ppm = rate_ppm();
    in_band = ppm >= cfg_.rate_min_ppm && ppm <= cfg_.rate_max_ppm;
    if (in_band) {
      rate_bad_run_ = 0;
      if (rate_ok_run_ < 255u) {
        ++rate_ok_run_;
      }
      if (refused_ && rate_ok_run_ >= cfg_.rate_hold_qf) {
        refused_ = false;
      }
    } else {
      rate_ok_run_ = 0;
      if (rate_bad_run_ < 255u) {
        ++rate_bad_run_;
      }
      if (!refused_ && rate_bad_run_ >= cfg_.rate_hold_qf) {
        refused_ = true;
      }
    }
  }
  // A master that is not followed: the pieces come backward, or the rate is out of the limits.
  if ((reverse_ || refused_) && (state_ == State::Acquired || running_state())) {
    const bool was_run = running_state();
    state_ = State::Reading;
    drop_mapping();
    if (was_run && n == Notice::None) {
      n = Notice::Stopped;
    }
  }
  if (state_ == State::Reading && known_ && dll_.running() && in_band && !refused_ && !reverse_) {
    state_ = State::Acquired;
    if (n == Notice::None) {
      n = Notice::Acquired;
    }
  }
  return n;
}

// One piece. Direction: consecutive ticks whose pieces climb are forward, whose pieces fall are backward (three in a row decide). Assembly: a cycle is pieces 0..7 on consecutive ticks; a
// dropped, repeated or out-of-order piece abandons it (the next piece 0 starts another).
void Chaser::assemble(std::uint32_t piece, std::uint32_t nibble, std::uint64_t tick, Notice& n) noexcept {
  if (have_prev_ && tick == prev_tick_ + 1u) {
    if (piece == ((prev_piece_ + 1u) & 7u)) {
      if (fwd_run_ < 255u) {
        ++fwd_run_;
      }
      rev_run_ = 0;
    } else if (piece == ((prev_piece_ + 7u) & 7u)) {
      if (rev_run_ < 255u) {
        ++rev_run_;
      }
      fwd_run_ = 0;
    }
    if (rev_run_ >= 3u) {
      reverse_ = true;
    } else if (fwd_run_ >= 3u) {
      reverse_ = false;
    }
  }
  have_prev_ = true;
  prev_piece_ = static_cast<std::uint8_t>(piece);
  prev_tick_ = tick;

  if (piece == 0u) {
    nib_[0] = static_cast<std::uint8_t>(nibble);
    asm_n_ = 1;
    asm_tick0_ = tick;
    return;
  }
  if (asm_n_ == piece && tick == asm_tick0_ + piece) {
    nib_[piece] = static_cast<std::uint8_t>(nibble);
    ++asm_n_;
    if (asm_n_ == 8u) {
      asm_n_ = 0;
      on_cycle(tick, n);
    }
  } else {
    asm_n_ = 0;
  }
}

// Eight pieces in order: the time at piece 0, plus two frames, is the master's time at the quarter frame after piece 7.
void Chaser::on_cycle(std::uint64_t tick_p7, Notice& n) noexcept {
  // Bits a piece does not use are zero in a real stream: anything else is noise, not a time.
  if ((nib_[1] & 0x0Eu) != 0u || (nib_[3] & 0x0Cu) != 0u || (nib_[5] & 0x0Cu) != 0u || (nib_[7] & 0x08u) != 0u) {
    ++stats_.bad_cycles;
    return;
  }
  const std::uint8_t code = static_cast<std::uint8_t>((nib_[7] >> 1) & 3u);
  const Time t{static_cast<std::uint8_t>(nib_[6] | ((nib_[7] & 1u) << 4)), static_cast<std::uint8_t>(nib_[4] | (nib_[5] << 4)),
               static_cast<std::uint8_t>(nib_[2] | (nib_[3] << 4)), static_cast<std::uint8_t>(nib_[0] | (nib_[1] << 4))};
  if (!label_valid(t, code)) {
    ++stats_.bad_cycles;
    return;
  }
  ++stats_.cycles;
  const std::uint32_t fpd = frames_per_day(code);
  const std::uint32_t qpd = 4u * fpd;
  const std::uint32_t n_dec = (frames_of_label(t, code) + 2u) % fpd;
  const std::uint32_t q_p7 = (4u * n_dec + qpd - 1u) % qpd;  // the number of this piece 7
  last_label_ = label_of_frames(n_dec, code);

  if (!known_ || code != code_) {
    // The first time, or the master changed its rate code (a different time base: a relocation for anyone following).
    const bool was_run = running_state();
    code_ = code;
    code_known_ = true;
    refused_ = false;  // the rate is judged again against the new time base
    rate_bad_run_ = 0;
    rate_ok_run_ = 0;
    set_map(tick_p7, q_p7);
    known_ = true;
    ++epoch_;
    if (was_run) {
      ++stats_.relocations;
      state_ = State::Reading;
      n = Notice::Relocated;
    }
    return;
  }
  const std::int32_t dq = wrap_diff(q_p7, q_of_tick(tick_p7), qpd);
  if (dq == 0) {
    return;
  }
  const std::uint32_t mag = static_cast<std::uint32_t>(dq < 0 ? -dq : dq);
  set_map(tick_p7, q_p7);  // the master's own count wins over the prediction
  ++epoch_;
  if (mag <= cfg_.reloc_qf) {
    ++stats_.resyncs;
    return;
  }
  ++stats_.relocations;
  if (state_ == State::Acquired || running_state()) {
    state_ = State::Reading;  // on_qf promotes it to Acquired at once: the cycle that showed the move was whole
    n = Notice::Relocated;
  }
}

Chaser::Notice Chaser::on_full(const MtcEvent& e, const ClockMap& map) noexcept {
  const Time t = unpack_time(e.value);
  const std::uint8_t code = unpack_code(e.value);
  if (!label_valid(t, code)) {
    return Notice::None;
  }
  ++stats_.full;
  const std::uint64_t at = sample_of(map, e.cycles);
  Notice n = Notice::None;
  if (state_ != State::Idle) {
    n = check_quiet(at);  // Stopped when the master had been silent: the news, with the locate below kept for locate()
  }
  if (state_ == State::Idle) {
    const std::uint32_t frames = frames_of_label(t, code);
    locate_.valid = true;
    locate_.time = qf_time_real(4u * frames, code);
    locate_.sample = at;
    locate_.label = t;
    locate_.code = code;
    last_label_ = t;
    ++stats_.locates;
    return n != Notice::None ? n : Notice::Locate;
  }
  // A full frame while quarter frames flow: the master located. Whatever was read is old; read again.
  const bool was_run = running_state();
  state_ = State::Reading;
  drop_mapping();
  if (was_run) {
    ++stats_.relocations;
    return Notice::Relocated;
  }
  return Notice::None;
}

bool Chaser::master_time_at(std::uint64_t sample, core::Q32& out) const noexcept {
  if (!known_ || !dll_.has_phase()) {
    return false;
  }
  const core::SQ32 period = static_cast<core::SQ32>(dll_.period());
  if (period <= 0) {
    return false;
  }
  const core::SQ32 rel = (static_cast<core::SQ32>(sample) - static_cast<core::SQ32>(dll_.origin())) * static_cast<core::SQ32>(core::kQ32One);
  const core::SQ32 dt = rel - dll_.time_of_tick(dll_.tick());
  const core::SQ32 k = floor_div(dt, period);  // whole quarter frames from the loop's last tick
  const core::SQ32 rem = dt - k * period;      // 0 .. period - 1
  const core::SQ32 qpd = static_cast<core::SQ32>(quarter_frames_per_day_of_code(code_));
  core::SQ32 q = (static_cast<core::SQ32>(q_of_tick(eff_tick())) + k % qpd) % qpd;
  if (q < 0) {
    q += qpd;
  }
  const core::Q32 nominal = qf_time_real(1, code_);
  const std::uint64_t ratio = (static_cast<std::uint64_t>(nominal) << kRatioFrac) / static_cast<std::uint64_t>(period);  // master time per local time
  out = qf_time_real(static_cast<std::uint32_t>(q), code_) + ((static_cast<std::uint64_t>(rem) * ratio) >> kRatioFrac);
  return true;
}

bool Chaser::sample_at_master_time(core::Q32 t, std::uint64_t& out) const noexcept {
  if (!known_ || !dll_.has_phase()) {
    return false;
  }
  const core::SQ32 period = static_cast<core::SQ32>(dll_.period());
  const core::Q32 nominal = qf_time_real(1, code_);
  if (period <= 0 || nominal == 0u || t / nominal >= (1u << 26)) {
    return false;
  }
  std::uint32_t q0 = static_cast<std::uint32_t>(t / nominal);  // within one of the quarter frame t is in (qf_time_real floors)
  while (q0 + 1u < (1u << 26) && qf_time_real(q0 + 1u, code_) <= t) {
    ++q0;
  }
  while (q0 > 0u && qf_time_real(q0, code_) > t) {
    --q0;
  }
  const core::Q32 rem = t - qf_time_real(q0, code_);  // 0 .. nominal - 1
  const std::uint64_t ratio = (static_cast<std::uint64_t>(period) << kRatioFrac) / static_cast<std::uint64_t>(nominal);  // local time per master time
  const core::SQ32 frac = static_cast<core::SQ32>((static_cast<std::uint64_t>(rem) * ratio) >> kRatioFrac);
  const std::uint32_t qpd = quarter_frames_per_day_of_code(code_);
  const std::int32_t dq = wrap_diff(q0 % qpd, q_of_tick(eff_tick()), qpd);
  if (dq > (1 << 21) || dq < -(1 << 21)) {
    return false;  // hours away
  }
  const core::SQ32 rel = dll_.time_of_tick(dll_.tick()) + static_cast<core::SQ32>(dq) * period + frac;
  const core::SQ32 whole = core::sq32_round(rel);
  const std::int64_t s = static_cast<std::int64_t>(dll_.origin()) + whole;
  out = s < 0 ? 0u : static_cast<std::uint64_t>(s);
  return true;
}

}  // namespace o3os::midi::mtc
