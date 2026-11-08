// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The master scratch stage (OS-DJ C1, docs/specs/OS-DJ.md section 2.6; the `headroom` slot of kMasterChain, the first stage: the ring holds the mix before the kill EQ, the filter and the limiter,
// which then act on the scratched sound; NEW COMMUNITY OS BEHAVIOUR, nothing is measured on an official unit).
//
// WHAT IT DOES. While armed it copies every block of the mix into a ring (16-bit stereo, up to 4 s: the sample arena's free tail, held by the app as the looper's take, Q7; stored at HALF scale, so a mix up
// to +-2.0 is kept whole: MAIN is float up to the limiter) and the output is a PLAY HEAD over that ring. Three drivers move the head, all as a RATE (1 = real time, negative = backwards) in frames per frame, and the head's distance behind the write head is its LAG:
//  - Platter (SCR on): the hand. The render reads the FTM2 counter once a block through a platform function pointer (Q8: 16 bits, atomic, signed wrap-safe difference). One count moves the head
//    kFramesPerCount frames (one turn of 96 counts is 1.8 s of audio). OS-SCRATCH: the hand's velocity is measured from the TIME between counts (in blocks: the last kHist count events), not from the
//    counts in a block, and the rate follows it by a one-pole of 70 ms (kInertia; OS-BUG-SCRATCH: tuneable at run time, set_tuning). A still encoder is a held record: the rate is 0 and the output is
//    silence. The record starts at rate 1 (it was playing when the hand came down), so the entry is the live mix running down to the hand through the one-pole: no step, silent (gate 0) after about 245 ms and exact silence (rate 0) after 336 blocks (0.9797^n < 0.001).
//  - Tape (TAPE STOP held): the rate falls from 1 to 0 in a straight line over the beat the app tells (the ParamChange's ramp), then the head is held.
//  - Spin (the backspin): SCR off with a backward rate of at least kSpinMin seen in the last 30 ms keeps the head going at that rate and brakes it in a straight line to 0 over 0.5 s.
// The sequencer runs underneath all the time (SLIP): the lag grows while the head is slower than real time, so SCR off or the release of TAPE STOP is live at the running position. The end of every driver
// is a RELEASE: a 256-sample crossfade from the head to the live mix, then the stage bypasses itself.
// The head never passes the write head (lag >= 0) and never reads outside what has been written (lag <= written - 2). The block is written whole before it is read, so on a full ring the lag stops kBlock + 2
// frames short of the ring's length: the oldest kBlock frames are already this block's. The lag is kept as an integer number of frames plus a fraction: a float of up to 176k frames resolves only 0.016 and
// would swallow the small increments of a rate near 1.
// The output is the head's interpolated sample (4-point Hermite for a Platter run, linear for the TAPE STOP: its output is unchanged since C1) times a gate g = min(slope |rate|, 1) (slope 32 for a Platter run, 8 for the TAPE STOP): a head at a standstill is silent, and a reversal passes 0 smoothly.
// OS-BUG-SCRATCH: a Platter run at |rate| > 1 reads through a one-pole low-pass (coefficient min(1, kAaCut / |rate|) of the block's larger end rate, one state a channel): bypassed, bit for bit, at |rate| <= 1.
//
// Real time: float only, no heap, no libm, one loop of exactly kBlock frames, the ring copy bounded by 64 frames; the FTM2 pointer is called once a block. In flash (O3OS_AUDIO_COLD), through the graph's
// MasterFn pointer; the block's cost is 0 when bypassed. Threads: stage() / unstage() / busy() are the APP thread's (the arena take is taken and given back only there); set_mode(), process(), abort() are the
// render's (the ParamChange drain and the chain are one context). The ring pointer is published by `staged_` (release) and read after it (acquire); `done_` hands the ring back: the app gives the arena up only
// after busy() is false, which is after the render has made its last access.

#include <o3os/core/audio_clock.h>
#include <o3os/hal/audio.h>

#include <atomic>
#include <cstdint>

namespace o3os::audio {

class MasterScratch {
 public:
  static constexpr std::uint32_t kBlock = hal::audio::kBlock;
  static constexpr std::uint32_t kMaxFrames = 176400;  // 4 s at 44.1 kHz: 705,600 bytes at 16-bit stereo
  static constexpr std::uint32_t kMinFrames = 4410;    // 0.1 s: a smaller tail is refused
  static constexpr float kRate = core::kRefRateHz;  // the engine's rate (core/audio_clock.h)
  static constexpr float kFramesPerCount = 1.8f * kRate / 96.0f;  // one turn (24 detents x 4 edges = 96 counts) is 1.8 s of audio
  static constexpr float kCountRate = kFramesPerCount / static_cast<float>(kBlock);  // the rate of one count in a block
  static constexpr int kMaxHand = 16;                  // counts a block: more is a glitch of the read, not a hand
  static constexpr float kBlockMs = 1000.0f * static_cast<float>(kBlock) / kRate;  // 1.45 ms
  static constexpr float inertia_for_ms(float ms) noexcept { return kBlockMs / (ms + kBlockMs); }  // the platter's one-pole per block for a time constant of `ms` (no libm: the first-order form of 1 - exp(-kBlockMs / ms))
  static constexpr float kInertia = kBlockMs / (70.0f + kBlockMs);  // = inertia_for_ms(70) (a member function is not usable in the class body); OS-BUG-SCRATCH: 70 ms (was 25: shorter than the gap between two detents at a slow hand, so the detent rate was heard as flutter); the run-time default
  static constexpr unsigned kHist = 8;                 // count events kept for the velocity (two detents: the 4 edges of a detent come in a burst, then a gap)
  static constexpr float kHold = 1.5f;                 // OS-BUG-SCRATCH: no count for `age` blocks caps the velocity at kHold / age counts a block: the last estimate holds for 1.5 times the inter-event time, then decays
  static constexpr float kFirstSpan = 8.0f;            // a lone count event (a start, or after a reversal) is taken as one count in this many blocks
  static constexpr std::uint32_t kStopBlocks = 96;     // blocks (139 ms) without a count event: the hand has stopped
  static constexpr float kPull = 1.0f / 4410.0f;       // rate per frame of debt beyond one count: the hand's position is paid back over about 100 ms
  static constexpr float kPullMax = 1.0f;              // the pay-back never takes the target beyond max(the hand's own rate, this): a steady hand is not overshot
  static constexpr float kAhead = 1.0f;                // OS-BUG-SCRATCH (was 0.25): counts the head may be projected beyond the last count (the hand is somewhere inside the count). A hand at 1x makes a count every 13 blocks:
                                                       // with less room the head ran out between counts and the rate fell to 0 (stop-start at the count rate). The run-time default
  static constexpr float kLoneAhead = 0.25f;           // a lone count event (no steady hand known yet) keeps the old reach: a click is a nudge, not a carry-on
  static constexpr float kDebtMax = 8.0f * kFramesPerCount;
  static constexpr float kSpinMin = 4.0f;              // a backward rate that counts as "turning fast" (about 0.3 counts a block)
  static constexpr unsigned kSpinWindow = 20;          // blocks: 29 ms ("the last 30 ms")
  static constexpr std::uint32_t kSpinSamples = 22059; // 0.5 s of brake
  static constexpr std::uint32_t kReleaseSamples = 256;
  static constexpr float kGateSlope = 8.0f;            // the TAPE STOP's gate (unchanged since C1)
  static constexpr float kPlatterGate = 32.0f;         // OS-BUG-SCRATCH: the Platter's gate (a run that began as SCR): silent only under 1/32 of real speed, a record slowed by hand is pitched down, not quiet. The run-time default
  static constexpr float kAaCut = 1.2f;                // OS-BUG-SCRATCH: above rate 1 the anti-alias one-pole's coefficient is min(1, kAaCut / |rate|)
  enum class Phase : std::uint8_t { Off, Platter, Tape, Spin, Release };
  using CountFn = std::uint16_t (*)() noexcept;  // the platform's read of the FTM2 counter (16 bits, wrapping)

  MasterScratch() noexcept = default;
  MasterScratch(const MasterScratch&) = delete;
  MasterScratch& operator=(const MasterScratch&) = delete;

  // The slot's bypass flag in the graph: cleared when a mode is taken, set when the stage has ended (render context).
  void attach(bool* bypass) noexcept { bypass_ = bypass; }
  void set_counter(CountFn fn) noexcept { count_ = fn; }  // once, before the render runs (the platform; a script on the host)

  // APP THREAD. Hands the ring (`frames` frames of interleaved 16-bit stereo, at most kMaxFrames used) to the stage. False when the stage is still busy or the ring is smaller than kMinFrames.
  // Follow it with the ParamChange (mode 1 or 2); if the ring refuses that push, unstage().
  bool stage(std::int16_t* ring, std::uint32_t frames) noexcept;
  void unstage() noexcept;
  // APP THREAD. True from stage() until the render has let go of the ring: the arena may not be given back (SampleBank::take_end) while true.
  bool busy() const noexcept { return staged_.load(std::memory_order_acquire) != done_.load(std::memory_order_acquire); }

  // RENDER. The ParamChange: 0 starts the end (a release or a backspin), 1 the platter (SCR), 2 TAPE STOP over `ramp_samples`, 3 (OS-DJSTOPLEN) a new length `ramp_samples` for the TAPE STOP that is braking
  // (the rate is kept, the rest of the brake runs over the new length). False for any other value, or a mode with no ring staged (3: no TAPE STOP in force).
  bool set_mode(float mode, std::uint32_t ramp_samples) noexcept;
  // OS-BUG-SCRATCH: the tuning, for listening on the unit (`diag scratch`). MAIN context writes, the render reads each value once a block into a local. Every field is one relaxed atomic (a plain aligned 32-bit
  // load or store, 8-bit for `aa`), so the render sees an old or a new value, never a torn one, and a block may mix old and new fields (each is valid alone). Session only, never stored. `inertia_ms` 5..500,
  // `ahead` 0..4 counts, `gate_slope` 1..64, `aa` the low-pass above rate 1; values outside the range (and NaN) are clamped.
  void set_tuning(float inertia_ms, float ahead, float gate_slope, bool aa) noexcept;
  float inertia_ms() const noexcept { return kBlockMs * (1.0f / inertia_.load(std::memory_order_relaxed) - 1.0f); }
  float ahead() const noexcept { return ahead_.load(std::memory_order_relaxed); }
  float gate_slope() const noexcept { return gate_.load(std::memory_order_relaxed); }
  bool aa() const noexcept { return aa_.load(std::memory_order_relaxed); }
  // RENDER (or with it parked): ends everything at once and lets go of the ring; the export's start.
  void abort() noexcept;
  bool idle() const noexcept { return phase_ == Phase::Off; }
  Phase phase() const noexcept { return phase_; }
  float rate() const noexcept { return rate_; }  // (for tests)
  float lag() const noexcept { return static_cast<float>(lag_i_) + lag_f_; }  // (for tests; to the float's resolution)
  std::int32_t lag_frames() const noexcept { return lag_i_; }
  float lag_fraction() const noexcept { return lag_f_; }
  // lag += `delta`, kept as `whole` frames and `frac` in [0, 1), held at 0 from below. (Public for the tests: it is what the render loop does every frame.)
  static void advance_lag(std::int32_t& whole, float& frac, float delta) noexcept {
    float f = frac + delta;
    if (f >= 1.0f || f < 0.0f) {
      std::int32_t n = static_cast<std::int32_t>(f);  // (|delta| is a rate: a few hundred at most)
      if (static_cast<float>(n) > f) {
        --n;  // the floor
      }
      whole += n;
      f -= static_cast<float>(n);  // exact: the fraction of f
      if (whole < 0) {
        whole = 0;
        f = 0.0f;
      }
    }
    frac = f;
  }

  // OS-SCRATCH: 4-point 3rd-order Hermite (Catmull-Rom) through `j` (t = 0) and `k` (t = 1), `m` the frame before `j` and `o` the one after `k`. Exact at t = 0 and 1 and for any signal up to a quadratic.
  // (Public for the tests: it is what the Platter's read does for each channel.)
  static float hermite4(float m, float j, float k, float o, float t) noexcept {
    const float c1 = 0.5f * (k - m);
    const float c2 = m - 2.5f * j + 2.0f * k - 0.5f * o;
    const float c3 = 0.5f * (o - m) + 1.5f * (j - k);
    return ((c3 * t + c2) * t + c1) * t + j;
  }

  // The largest lag the read may use once `seen` frames of the run have been written, on a ring of `cap` frames: two behind the newest frame (the interpolation reads one more), and `ring_lim` on a ring that is
  // full or about to wrap. (Public for the tests.)
  static std::int32_t read_limit_for(std::uint32_t cap, std::uint32_t seen, std::int32_t ring_lim) noexcept {
    const std::uint32_t avail = seen < cap ? seen : cap;
    const std::int32_t lim = avail >= 2u ? static_cast<std::int32_t>(avail - 2u) : 0;
    return lim < ring_lim ? lim : ring_lim;
  }
  // OS-SCRATCH-IDLE / IDLE2: the lag after one block at rate 0 (+1 frame a frame), `before` frames written when the block starts. In closed form, bit for bit what the per-sample loop leaves. (Public for the tests,
  // which hold it against that loop.)
  static void still_lag(std::uint32_t cap, std::uint32_t before, std::int32_t ring_lim, std::int32_t& lag_i, float& lag_f) noexcept;

  // One block of exactly kBlock frames, in place. Any other n is left untouched. The graph's MasterFn is process_fx.
  void process(float* left, float* right, std::uint32_t n) noexcept;
  static void process_fx(void* self, float* left, float* right, std::uint32_t n) noexcept { static_cast<MasterScratch*>(self)->process(left, right, n); }

 private:
  void write_block(const float* left, const float* right) noexcept;
  std::int32_t read_limit(std::uint32_t seen, std::int32_t ring_lim) const noexcept { return read_limit_for(cap_, seen, ring_lim); }
  float next_rate() noexcept;
  float platter_target(int hand) noexcept;
  void begin_release() noexcept;
  void finish() noexcept;

  std::int16_t* ring_ = nullptr;
  std::uint32_t cap_ = 0;  // frames
  std::atomic<std::uint32_t> staged_{0};
  std::atomic<std::uint32_t> done_{0};
  std::uint32_t epoch_ = 0;  // the staged_ value this run belongs to
  CountFn count_ = nullptr;
  bool* bypass_ = nullptr;
  Phase phase_ = Phase::Off;
  bool exit_req_ = false;
  std::uint32_t widx_ = 0;     // ring index of the next frame to write
  std::uint32_t written_ = 0;  // frames written, saturating at cap_
  std::int32_t lag_i_ = 0;     // frames behind the write head: the whole frames ...
  float lag_f_ = 0.0f;         // ... and the fraction, in [0, 1)
  float rate_ = 0.0f;
  std::uint16_t last_count_ = 0;
  std::uint32_t t_ = 0;        // samples into Tape / Spin
  std::uint32_t len_ = 1;      // Tape: the beat, in samples
  // OS-SCRATCH, the Platter's velocity (render only: set_mode and process are one context). Reset at every set_mode.
  std::uint32_t blk_ = 0;                       // blocks since the platter began
  std::uint32_t ev_blk_[kHist] = {};            // the block index of the last count events, newest first ...
  std::int16_t ev_n_[kHist] = {};               // ... and their signed counts
  unsigned n_ev_ = 0;
  std::int8_t dir_ = 0;                         // the sign of the newest event
  float debt_ = 0.0f;                           // frames: the hand's counts minus what the targets have played, along dir_
  float hand_rate_ = 0.0f;                      // the hand's own target of the last block (before the inertia), for the backspin latch
  bool hermite_ = false;                        // a Platter run reads with the 4-point Hermite; the TAPE STOP keeps the linear read
  // OS-BUG-SCRATCH: the tuning (main writes, render reads: set_tuning) and the anti-alias low-pass's state (render only; zeroed at set_mode, follows the read while the filter is bypassed so it engages without a step).
  std::atomic<float> inertia_{kInertia};
  std::atomic<float> ahead_{kAhead};
  std::atomic<float> gate_{kPlatterGate};
  std::atomic<bool> aa_{true};
  float aa_l_ = 0.0f;
  float aa_r_ = 0.0f;
  float spin_rate_ = 0.0f;
  float back_rate_ = 0.0f;     // the last backward rate at or beyond kSpinMin
  unsigned back_age_ = 255;    // blocks since
  std::uint32_t fade_ = 0;     // samples into the release
};

}  // namespace o3os::audio
