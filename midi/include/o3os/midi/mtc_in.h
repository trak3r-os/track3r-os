// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// MIDI Time Code in: the chaser (OS-MTC B2; docs/specs/OS-MTC.md 4.1 and 4.2). Pure render-context logic: integer and Q32 arithmetic (the loop's gains are ClockDll's single-precision
// floats), no allocation, no I/O, no double, bounded work per call. New Community OS behaviour: the official firmware follows no MTC. The formats are the MIDI 1.0 Detailed
// Specification's, "MIDI Time Code". NOT wired to the sequencer: B3 reads the chaser's states and answers its requests; nothing here starts or stops a run.
//
// WHAT COMES IN. MidiIn::drain hands over MtcEvents (midi/in.h): a quarter frame (its data byte `0nnn dddd`) or a full frame (the packed time), each with the cycle stamp of the moment the
// sender began it, in order. feed() takes one, poll() is called once a block with the block's first sample (silence is noticed there, and by the next message that comes late).
//
// THE TIME. Quarter frame number q counts from 00:00:00:00 of the master's day (4 per frame, 4 x fps a second; 29.97 drop-frame counts frames as the label counts them minus the dropped
// ones, and runs at 30000/1001 frames a second in real time). The master time of q is q x (samples per quarter frame): the real time, in Q32 samples, that the label stands for at
// the engine's sample rate; a day is below 2^32 samples. Pieces 0..7 of a cycle spell the frame at which piece 0 was sent; the cycle is whole at piece 7 and the time it stands at is that
// plus two frames, the quarter frame after piece 7: so piece 7's number is 4 x (frame + 2) - 1.
//
// THE LOOP. Every quarter frame's stamp feeds a second ClockDll (the quarter frame is its "clock"): its fit, its slewing loop and its dropout rules are the clock follow's. Its tick
// counts quarter frames the master sent, dropped ones included, so the mapping tick -> q is one number (q of a reference tick) and a dropped piece costs nothing but its own check.
// From the loop and the mapping come master_time_at(sample) and its inverse sample_at_master_time(T), and the master's rate against the unit's audio clock (rate_ppm()).
//
// STATES (spec 4.2). Idle (no quarter frame) -> Reading (the first one arrived; pieces collected in order from piece 0, the loop fitting) -> Acquired (a whole cycle decoded, the loop
// running, the rate inside the limits) -> Armed (B3 scheduled the start: arm()) -> Locked (B3's run agrees with the master: lock()). Freewheel is Armed or Locked with no quarter frame
// for more than freewheel_qf periods (the run goes on at the last rate; the next quarter frame returns). No quarter frame for stop_qf periods (4 frames) is the master stopping: Idle,
// and the notice Stopped. A relocation is a decoded time that disagrees with the mapping by more than one frame (or a rate code that changed, or a full frame while Armed, Locked or
// Freewheel): the notice Relocated, and the chaser reads the new time (Acquired again at once when the cycle that showed it was whole). Pieces that come in reverse order (the master
// shuttles backward) are not followed; a rate outside the limits is not followed (refused()): the state stays Reading, and a run that was Armed or Locked gets Stopped.
//
// SLIP. A master that has slowed to gaps of 1.5 to 2.5 periods (0.4 to 0.67 of its rate) looks to the loop like a master that drops every second quarter frame: each one is a
// "dropout", the loop's error stays near zero, its period never moves and its tick advances by two (or by one and two by turns), so neither the rate gate nor the piece assembly sees it; a
// master that speeds up is followed by the loop only as fast as its bandwidth lets it. In all of them the loop's tick and the master's piece number part company: between two quarter frames
// taken one after the other, the ticks have advanced by the number of quarter frames the master sent, and the pieces must have advanced by the same number (modulo 8). The chaser checks
// that, once it has a mapping: slip_qf quarter frames among the last 64 whose piece step is not the tick step (a master that loses one in eight, with or without jitter, has none) is not
// a master it can follow. Stopped for a run, and the reading starts again from this quarter frame (the loop is reset, so it fits the new gaps and the rate gate refuses them, or follows
// them if they come back to a rate it accepts). A reverse run is not a slip (its pieces fall by one, and are told by `reverse()`).
//
// THE TICK. The mapping counts the loop's ticks. The loop's fit takes a gap of two periods for one tick (its "odd" path, or a long second interval); the chaser sees such a gap in the
// stamps (a gap of 2 or more of its periods while the loop is not running yet: for the second quarter frame, the nominal one) and restarts the fit from that quarter frame itself, counting
// the quarter frames the gap held, so the tick the mapping uses (tick_base + the loop's) is the master's count and the fit is never made of a gap. This is only for a reading that has no mapping yet: under a mapping a loop that is fitting again has lost the master (the reading restarts), so a restart can never shift a mapping or wipe the history of a run that is going well. Three restarts in a row are a master
// that is slow rather than one that loses quarter frames: the loop is left to fit it, and the rate gate refuses it.
//
// EPOCH. Every change of the mapping (a first cycle, a resync, a relocation, a stop, a re-read) bumps epoch(); B3 stamps its request with it and drops an answer that carries an old one.
//
// WHAT IS NOT CHECKED. A quarter frame in reverse is told from a forward one by the piece order of consecutive ticks (three in a row decide); a master that sends only some pieces never
// completes a cycle, so its time is never verified (the loop's count still follows it, and a relocation shows only when a whole cycle does).

#include <o3os/core/qtime.h>
#include <o3os/midi/clock_dll.h>
#include <o3os/midi/in.h>
#include <o3os/midi/mtc.h>

#include <cstdint>

namespace o3os::midi::mtc {

// ---- rate codes and labels (the wire's, 29.97 drop-frame included) ----------------------------------------------------------------------------
inline constexpr std::uint8_t kCodeDrop = 2;  // 0 = 24, 1 = 25, 2 = 29.97 drop-frame, 3 = 30 (rate_code() in mtc.h)

// The frames a label counts to a second (30 for drop-frame: it counts 30 and skips labels).
constexpr std::uint32_t fps_of_code(std::uint8_t code) noexcept { return (code & 3u) == 0u ? 24u : ((code & 3u) == 1u ? 25u : 30u); }
// Frame labels in a day: 24 hours at the rate; drop-frame skips frames 00 and 01 of every minute but each tenth: 2,592,000 - 2 x (1440 - 144).
constexpr std::uint32_t frames_per_day(std::uint8_t code) noexcept { return (code & 3u) == kCodeDrop ? 2589408u : kSecondsPerDay * fps_of_code(code); }
constexpr std::uint32_t quarter_frames_per_day_of_code(std::uint8_t code) noexcept { return 4u * frames_per_day(code); }

// A label that exists at the rate code: fields in range, and for drop-frame not frames 00 and 01 of a minute that is not a tenth.
constexpr bool label_valid(const Time& t, std::uint8_t code) noexcept {
  if (t.h > kMaxHour || t.m > 59u || t.s > 59u || t.f >= fps_of_code(code)) {
    return false;
  }
  return !((code & 3u) == kCodeDrop && t.s == 0u && t.f < 2u && (t.m % 10u) != 0u);
}
// The frames elapsed from 00:00:00:00 at a valid label (drop-frame: the labels skipped are not counted).
constexpr std::uint32_t frames_of_label(const Time& t, std::uint8_t code) noexcept {
  std::uint32_t n = frame_of_time(t, fps_of_code(code));
  if ((code & 3u) == kCodeDrop) {
    const std::uint32_t tm = static_cast<std::uint32_t>(t.h) * 60u + t.m;
    n -= 2u * (tm - tm / 10u);
  }
  return n;
}
// The label of a frame count (wrapping at the day): the inverse.
constexpr Time label_of_frames(std::uint32_t n, std::uint8_t code) noexcept {
  n %= frames_per_day(code);
  if ((code & 3u) == kCodeDrop) {
    const std::uint32_t d = n / 17982u;  // ten minutes of drop-frame: 17,982 frames
    const std::uint32_t m = n % 17982u;
    n += 18u * d + (m >= 2u ? 2u * ((m - 2u) / 1798u) : 0u);
  }
  return time_of_frame(n, fps_of_code(code));
}

// Master time of quarter frame `q` (below 2^26) from 00:00:00:00, Q32 samples at the master's real rate: A3's exact value; drop-frame runs 0.1 % slower than 30 fps.
constexpr core::Q32 qf_time_real(std::uint32_t q, std::uint8_t code) noexcept {
  const core::Q32 t = qf_time_q32(q, fps_of_code(code));
  return (code & 3u) == kCodeDrop ? t + t / 1000u : t;
}

// ---- the chaser -------------------------------------------------------------------------------------------------------------------------
// The loop's settings for quarter frames (they come 96 to 120 a second, a tenth of the MIDI clock's period): the fit is 8 of them (spec 4.2), the re-acquire band is wide (60 % of a period and
// beyond 4 ms: a +-2 ms jittered master's gaps swing 40 % of a period, which would restart the fit), and the leaky score of missing pieces never re-acquires (its take-back would shift the
// piece count under a master that drops pieces; a rate that is not followed is refused by the chaser instead).
inline ClockDll::Config default_loop_config() noexcept {
  ClockDll::Config c;
  c.fit_clocks = 8;
  c.reacquire_pct = 60;
  c.drop_score_max = 0x7FFFFFFFu;
  return c;
}

class Chaser {
 public:
  enum class State : std::uint8_t { Idle, Reading, Acquired, Armed, Locked, Freewheel };
  // What a call tells B3 (at most one per call).
  enum class Notice : std::uint8_t {
    None,
    Acquired,   // time known and the loop fitted: a request can be made (master_time_at)
    Relocated,  // the master moved (or changed rate code) under an Armed, Locked, Freewheel or Acquired chaser: stop the run, request again once Acquired
    Stopped,    // the master stopped (no quarter frame for stop_qf periods), shuttled backward or left the rate limits: the run must stop. State: Idle, or Reading for the last two
    Locate,     // a full frame with the chaser Idle: the master located; the time is locate()
  };

  struct Config {
    ClockDll::Config loop = default_loop_config();
    std::uint32_t freewheel_qf = 4;         // no quarter frame for more than this many periods (one frame) while Armed or Locked: Freewheel
    std::uint32_t stop_qf = 16;             // ... for more than this many (four frames): the master stopped
    std::uint32_t reloc_qf = 4;             // a decoded time off the mapping by more than this many quarter frames (one frame) is a relocation; up to it, a resync
    std::int32_t rate_min_ppm = -60000;     // the master's rate against the unit's clock, parts per million from 1.0: 0.94 ..
    std::int32_t rate_max_ppm = 60000;      // .. 1.06
    std::uint32_t rate_hold_qf = 16;        // quarter frames outside (inside) the limits before the rate is refused (accepted again): the loop's overshoot after an abrupt step inside the limits is ridden out (a +4 % step overshoots to 8 % for about 10)
    std::uint32_t slip_qf = 4;              // quarter frames among the last 64 whose piece step is not the loop's tick step: the master is not followable (see SLIP above)
  };

  struct Locate {
    bool valid = false;
    core::Q32 time = 0;       // the master time the full frame names, Q32 samples
    std::uint64_t sample = 0; // the timeline sample at which the master sent it
    Time label;
    std::uint8_t code = 0;
  };

  struct Stats {
    std::uint32_t qf = 0;          // quarter frames taken
    std::uint32_t ignored = 0;     // too early to be the next one (a glitch)
    std::uint32_t full = 0;        // full frames taken
    std::uint32_t cycles = 0;      // whole cycles decoded
    std::uint32_t bad_cycles = 0;  // whole cycles whose fields were not a time
    std::uint32_t resyncs = 0;     // a decoded time off the mapping by up to a frame: the decoded time was taken
    std::uint32_t relocations = 0;
    std::uint32_t stops = 0;       // master silence
    std::uint32_t locates = 0;
    std::uint32_t slips = 0;       // readings restarted because the loop's tick and the master's pieces parted (SLIP)
    std::uint32_t refits = 0;      // fits restarted because a quarter frame was missing in them (THE TICK)
    std::uint32_t fit_losses = 0;  // the loop was fitting again under a mapping: the reading restarted (THE TICK)
  };

  Chaser() = default;
  explicit Chaser(const Config& c) : cfg_(c) { dll_.configure(c.loop); }
  void configure(const Config& c) noexcept {
    cfg_ = c;
    dll_.configure(c.loop);
  }
  const Config& config() const noexcept { return cfg_; }
  // Forget everything (state Idle). The counters are kept.
  void reset() noexcept;

  // One MtcEvent from MidiIn::drain, in order; `map` is the block's stamp map.
  Notice feed(const MtcEvent& e, const ClockMap& map) noexcept;
  // Once a block, with the block's first sample: silence (Freewheel, then the stop).
  Notice poll(std::uint64_t now_sample) noexcept;

  // ---- B3's side: the run's state (the chaser never starts or stops anything) ----------------------------------------------------------------
  // Acquired -> Armed when `epoch` is still the mapping the start was computed from; false (and no change) otherwise.
  bool arm(std::uint32_t epoch) noexcept {
    if (state_ == State::Acquired && epoch == epoch_) {
      state_ = State::Armed;
      return true;
    }
    return false;
  }
  // Armed -> Locked: the run agrees with the master (spec 4.4: |e| below 1 ms for 8 quarter frames, B3's measure).
  bool lock() noexcept {
    if (state_ == State::Armed) {
      state_ = State::Locked;
      return true;
    }
    return false;
  }
  // Armed, Locked or Freewheel -> Acquired: the run ended on the unit's side (the user's Stop, a start that came too late); the chaser keeps reading.
  void disarm() noexcept {
    if (state_ == State::Armed || state_ == State::Locked || state_ == State::Freewheel) {
      state_ = State::Acquired;
    }
  }

  // ---- what B3 and the status line read ------------------------------------------------------------------------------------------------------
  State state() const noexcept { return state_; }
  bool refused() const noexcept { return refused_; }    // the rate is outside the limits (status MTC RATE)
  bool reverse() const noexcept { return reverse_; }    // the pieces come backward
  std::uint32_t epoch() const noexcept { return epoch_; }
  bool time_known() const noexcept { return known_; }
  std::uint8_t code() const noexcept { return code_; }
  std::uint32_t fps() const noexcept { return fps_of_code(code_); }
  Time last_time() const noexcept { return last_label_; }  // the time the last whole cycle stands at (the label of the frame two after piece 0)
  // The master's rate against the unit's audio clock, parts per million from 1.0 (positive: the master is fast). 0 until the loop runs.
  std::int32_t rate_ppm() const noexcept;
  const Locate& locate() const noexcept { return locate_; }
  const ClockDll& loop() const noexcept { return dll_; }
  const Stats& stats() const noexcept { return stats_; }

  // The master's time at the timeline sample `sample`, Q32 samples from 00:00:00:00 (real time, within a day), from the loop's prediction; false until the time is known and the loop has a phase.
  bool master_time_at(std::uint64_t sample, core::Q32& out) const noexcept;
  // The inverse: the timeline sample (nearest) at which the master reaches `t` (Q32 samples within a day). False when unknown or when `t` is more than a few hours from now.
  bool sample_at_master_time(core::Q32 t, std::uint64_t& out) const noexcept;

 private:
  Notice on_qf(const MtcEvent& e, const ClockMap& map) noexcept;
  Notice on_full(const MtcEvent& e, const ClockMap& map) noexcept;
  Notice check_quiet(std::uint64_t now_sample) noexcept;
  void assemble(std::uint32_t piece, std::uint32_t nibble, std::uint64_t tick, Notice& n) noexcept;
  void on_cycle(std::uint64_t tick_p7, Notice& n) noexcept;
  void set_map(std::uint64_t tick, std::uint32_t q) noexcept;
  std::uint32_t q_of_tick(std::uint64_t tick) const noexcept;
  void drop_mapping() noexcept;
  void begin_reading(const ClockMap& map) noexcept;
  std::uint64_t eff_tick() const noexcept { return tick_base_ + dll_.tick(); }
  void to_idle() noexcept;
  bool running_state() const noexcept { return state_ == State::Armed || state_ == State::Locked || state_ == State::Freewheel; }

  Config cfg_{};
  ClockDll dll_{default_loop_config()};
  State state_ = State::Idle;
  State resume_ = State::Armed;       // the state a Freewheel returns to
  std::uint32_t epoch_ = 0;
  std::uint64_t last_sample_ = 0;     // the timeline sample of the last quarter frame taken
  std::uint32_t last_cycles_ = 0;     // its stamp (corrected)
  std::uint64_t tick_base_ = 0;       // the master's quarter-frame count of the loop's tick 0 (the loop's fit is restarted by the chaser: THE TICK)
  std::uint8_t refit_run_ = 0;        // fit restarts in a row, one for each quarter frame: a master that is slow, not one that loses quarter frames (THE TICK)
  std::uint64_t slip_hist_ = 0;       // one bit per quarter frame, newest lowest: its piece step was not the tick step (SLIP)

  // the assembler of a cycle
  std::uint8_t nib_[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  std::uint8_t asm_n_ = 0;            // pieces collected, from piece 0 in order and on consecutive ticks
  std::uint64_t asm_tick0_ = 0;
  bool have_prev_ = false;
  std::uint8_t prev_piece_ = 0;
  std::uint64_t prev_tick_ = 0;
  std::uint8_t fwd_run_ = 0;
  std::uint8_t rev_run_ = 0;
  bool reverse_ = false;

  // the mapping: quarter frame `q_ref_` of the master's day is the loop's tick `tick_ref_`
  bool known_ = false;
  bool code_known_ = false;           // a whole cycle has named the rate code (kept while a mapping is dropped, so that a refused rate can be accepted again)
  std::uint8_t code_ = 1;
  std::uint32_t q_ref_ = 0;
  std::uint64_t tick_ref_ = 0;
  Time last_label_;

  // the rate
  bool refused_ = false;
  std::uint8_t rate_bad_run_ = 0;
  std::uint8_t rate_ok_run_ = 0;

  Locate locate_{};
  Stats stats_{};
};

}  // namespace o3os::midi::mtc
