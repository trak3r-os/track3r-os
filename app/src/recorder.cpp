// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/recorder.h>

#include <cstring>

namespace o3os::app {

namespace {

namespace cap = hal::capture;
using audio::RecTap;
using audio::route::kCue;
using audio::route::kMain;
using audio::route::kRec;
using audio::route::kUsb;

constexpr unsigned kTapChoices = 2u + kMixTracks;  // MASTER, REC BUS, TRACK 1..16

int clamp_int(int v, int lo, int hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

unsigned tap_index(const RecorderConfig& cfg) noexcept {
  return cfg.tap == RecTapKind::Master ? 0u : (cfg.tap == RecTapKind::RecBus ? 1u : 2u + cfg.track);
}

}  // namespace

EnginePlan make_plan(const RecorderConfig& cfg, RecMode mode, bool owned, bool settling, const MixBoard* board) noexcept {
  EnginePlan p;
  // The Mixer's own values, or the engine's defaults (graph.cpp init_defaults) without a board.
  for (unsigned i = 0; i < kMixTracks; ++i) {
    p.route[i] = board != nullptr ? board->route[i] : static_cast<std::uint8_t>(kMain | kUsb);
  }
  p.route[audio::kInputStrip] = board != nullptr ? board->route[audio::kInputStrip] : static_cast<std::uint8_t>(kMain | kCue);
  p.in_level = board != nullptr ? board->in_level : 0u;
  if (!owned) {
    return p;
  }
  if (mode == RecMode::Resample) {
    // What the engine plays. The input strip is not the source: its feed is the Mixer's. MASTER is MAIN after the master level (the REC bus is not involved); REC BUS is what the
    // Routing page sends to REC; TRACK n sends that one track (and no other strip) to REC, post-fader and through its pan (a track panned off centre records at the mean of its two
    // sides: the take is mono).
    switch (cfg.tap) {
      case RecTapKind::Master:
        p.rec_source = RecTap::Source::Master;
        break;
      case RecTapKind::RecBus:
        break;
      case RecTapKind::Track:
        for (unsigned i = 0; i < kMixTracks; ++i) {
          p.route[i] = i == cfg.track ? static_cast<std::uint8_t>(p.route[i] | kRec) : static_cast<std::uint8_t>(p.route[i] & ~kRec);
        }
        p.route[audio::kInputStrip] = static_cast<std::uint8_t>(p.route[audio::kInputStrip] & ~kRec);
        break;
    }
    return p;
  }
  // An input: the input strip alone feeds REC (no track does), at unity and held silent while a source change settles; it is heard only if the monitor is on (MAIN and CUE).
  const SourceInfo& info = source_info(mode_source(cfg, mode));
  p.feed = info.feed;
  p.channel = info.channel;
  p.in_level = settling ? 0u : 100u;
  for (unsigned i = 0; i < kMixTracks; ++i) {
    p.route[i] = static_cast<std::uint8_t>(p.route[i] & ~kRec);
  }
  p.route[audio::kInputStrip] = static_cast<std::uint8_t>(kRec | (cfg.monitor ? (kMain | kCue) : 0));
  return p;
}

std::uint32_t Recorder::tenths(std::uint32_t frames) noexcept {
  const std::uint64_t mhz = hal::audio::sample_rate_mhz();  // the engine's rate in millihertz
  return static_cast<std::uint32_t>((static_cast<std::uint64_t>(frames) * 10000u + mhz / 2u) / mhz);
}

unsigned Recorder::meter_channel() const noexcept {
  const bool master = mode_ == RecMode::Resample && cfg.tap == RecTapKind::Master;
  return meter_left(master ? RecTap::Source::Master : RecTap::Source::Bus);
}

// ---- The engine's side ---------------------------------------------------------------------------------------------------------------------

bool Recorder::push_param(core::ParamId id, float value, std::uint32_t ramp) noexcept {
  return w_.params != nullptr && w_.params->push(core::ParamChange{id, value, ramp});
}

// Tells the engine what `want` says where it differs from what it was last told. The graph's atomics at once, the ParamChanges as far as the ring takes them; true when
// everything is where it should be (a full ring leaves the rest for the next poll).
bool Recorder::push_plan(const EnginePlan& want) noexcept {
  audio::Graph& g = *w_.graph;
  if (sent_.feed != want.feed) {
    g.set_input_feed(want.feed);
    sent_.feed = want.feed;
  }
  if (sent_.channel != want.channel) {
    g.rec().set_channel(want.channel);
    sent_.channel = want.channel;
  }
  if (sent_.rec_source != want.rec_source) {
    g.rec().set_source(want.rec_source);
    sent_.rec_source = want.rec_source;
  }
  bool all = true;
  for (unsigned i = 0; i < kMixStrips; ++i) {
    if (sent_.route[i] != want.route[i]) {
      if (push_param(audio::strip_param(i, audio::graph_param::strip::Route), static_cast<float>(want.route[i]), 1u)) {
        sent_.route[i] = want.route[i];
      } else {
        all = false;
      }
    }
  }
  if (sent_.in_level != want.in_level) {
    if (push_param(audio::strip_param(audio::kInputStrip, audio::graph_param::strip::Volume), static_cast<float>(want.in_level), 0u)) {
      sent_.in_level = want.in_level;
    } else {
      all = false;
    }
  }
  return all;
}

void Recorder::apply(std::uint64_t now_ms, bool playing) noexcept {
  (void)playing;
  const bool own = in_view_ || state_ == State::Armed || state_ == State::Recording || ov_wanted_ || (loop_review_ && w_.graph->loop().overdubbing());  // (OS-LOOPER L2c: a session keeps the input on any screen)
  if (own && !owned_) {
    plan_known_ = false;  // the engine is where the Mixer left it: the board's values
    hw_valid_ = false;    // the source is selected again on entry (the analogue switch and the gain follow the config)
  }
  if (!own && !owned_ && !plan_known_) {
    return;  // never owned: the engine was not touched
  }
  if (!plan_known_) {
    sent_ = make_plan(cfg, mode_, false, false, w_.mix);
    plan_known_ = true;
  }
  owned_ = own;

  if (own && mode_ != RecMode::Resample && w_.hw != nullptr) {
    const cap::Source src = mode_source(cfg, mode_);
    const std::uint8_t gain = gain_of(src);
    if (!hw_valid_ || src != hw_source_) {
      if (now_ms >= hw_retry_at_) {
        if (src == cap::Source::Radio) {
          w_.hw->radio_region(cfg.region);  // the receiver comes up on what the config says
          w_.hw->radio_tune(cfg.radio_freq);
        }
        if (w_.hw->select_source(src, gain)) {
          hw_valid_ = true;
          hw_source_ = src;
          hw_gain_ = gain;
          settle_until_ = now_ms + kSourceSettleMs;  // the ADC's input switch clicks: the input is held silent meanwhile
          if (note_ == Note::CodecFailed) {
            note_ = Note::None;
          }
        } else {
          hw_retry_at_ = now_ms + 500u;
          note_ = Note::CodecFailed;
        }
        touch();
      }
    } else if (gain != hw_gain_ && source_info(src).gain_bank != kNoGain) {
      if (w_.hw->set_gain(src, gain)) {
        hw_gain_ = gain;
      }
    }
  } else if (own && w_.hw == nullptr) {
    hw_valid_ = true;  // no board to point (the simulator, a test): the engine's side is all there is
  }

  const bool settling = now_ms < settle_until_;
  if (settling != settling_) {
    settling_ = settling;
    touch();
  }
  const EnginePlan want = make_plan(cfg, mode_, own, settling_, w_.mix);
  const bool done = push_plan(want);
  dirty_ = !done;
  if (!own && done) {
    plan_known_ = false;  // given back: the engine is the board's again
  }
}

// ---- The poll -----------------------------------------------------------------------------------------------------------------------------

void Recorder::rebuild_view() noexcept {
  view_epoch_ = ov_.epoch();
  if (ov_.samples() == 0u || !view_.init(view_store_, kViewElems, ov_.data(), ov_.samples(), 1)) {
    view_ = ui::plot::PeakCache{};
    return;
  }
  view_.build();  // at most 4,096 samples: a few thousand reads
}

void Recorder::on_bank_clear(void* self) noexcept {
  Recorder& r = *static_cast<Recorder*>(self);
  if (r.w_.graph == nullptr) {
    return;
  }
  if (r.loop_take_ || r.loop_review_) {
    r.w_.graph->loop().release();  // poll() sees the arena gone and gives the take up (Note::Lost)
  }
  if (r.state_ == State::Armed || r.state_ == State::Recording) {  // (Armed covers a take that waits for its step and one that listens for its level)
    r.w_.graph->rec().disarm();  // the take must not write over what a loader puts there next
  }
}

bool Recorder::bank_clear_busy(void* self) noexcept {
  const Recorder& r = *static_cast<const Recorder*>(self);
  if (r.w_.graph == nullptr) {
    return false;
  }
  const audio::RecTap::State ts = r.w_.graph->rec().state();
  return r.w_.graph->loop().phase() != audio::LoopPlayer::Phase::Idle || ts == audio::RecTap::State::Armed || ts == audio::RecTap::State::Waiting || ts == audio::RecTap::State::Listening || r.w_.graph->scratch().busy();  // (OS-DJ C1: the scratch ring is the other owner of the tail)
}

void Recorder::give_up_take() noexcept {
  take_ = nullptr;
  frames_ = 0;
  capacity_ = 0;
  trim_start_ = 0;
  floor_ = 0;
  trim_end_ = 0;
  ov_.reset();
}

void Recorder::finish() noexcept {
  RecTap& tap = w_.graph->rec();
  if (loop_take_ && w_.graph->loop().capturing()) {
    return;  // the render has not yet decided whether this take is a loop (it does, in the block that ended the take): the next poll
  }
  const std::uint32_t frames = tap.frames();
  // (a loop take that ends exactly on the last free frame is a loop: only a frame that did not fit makes it full)
  const bool full = tap.overrun_frames() != 0u || (!loop_take_ && frames >= capacity_);
  const bool looped = loop_take_ && w_.graph->loop().reading();  // the take ended on its boundary and the loop plays it
  const bool held = w_.bank->take_held();
  const bool level = wait_align_ == RecTap::Align::Level;  // (set by arm(); a loop take never is)
  if (level && held) {
    floor_ = tap.settle_ring();  // OS-SREC-LEVEL: the ring is put in time order (poll() did it at the trigger; here for a take that ended before a poll saw it), only inside a take that is still ours
  }
  tap.release();
  dirty_ = true;  // the take no longer holds the engine's input path: the poll applies the plan again (the screen may be gone)
  if (!held) {  // a project load emptied the arena under the take: it is gone
    stop_pending_ = false;
    state_ = State::Idle;
    note_ = Note::Lost;
    if (loop_take_) {
      w_.graph->loop().release();
      loop_take_ = false;
    }
    give_up_take();
    touch();
    return;
  }
  if (take_ != nullptr) {
    if (level) {
      ov_.reset();  // OS-SREC-LEVEL: whatever was fed from the ring before it was in order is read again
    }
    ov_.feed(take_, frames, 0xFFFFFFFFu);
  }
  stop_pending_ = false;
  if (loop_take_ && !looped) {  // a loop take that did not end on its N-th bar is no loop: its memory goes back, with a note
    loop_take_ = false;
    w_.bank->take_end(0);
    state_ = State::Idle;
    note_ = full ? Note::Full : (frames != 0u ? Note::LoopCut : Note::None);
    give_up_take();
    touch();
    return;
  }
  w_.bank->take_end(frames, looped ? frames + SampleBank::undo_bitmap_samples(frames) : 0u);  // a loop keeps room for its undo buffer (as long as the loop) and the bitmap behind the take (L2)
  loop_review_ = looped;
  loop_take_ = false;
  frames_ = frames;
  if (floor_ >= frames) {  // (a level take that holds nothing but its pre-roll: never the case for a trigger, which keeps its own frame)
    floor_ = 0;
  }
  trim_start_ = floor_;  // OS-SREC-LEVEL: the marker starts where the audio does (the frames before it are zeros); the user's own marker is never used for it
  trim_end_ = frames;
  state_ = frames != 0u ? State::Review : State::Idle;
  note_ = full ? Note::Full : Note::None;
  touch();
}

bool Recorder::poll(std::uint64_t now_ms, bool in_view, RecMode mode, bool playing, bool live_idle) noexcept {
  const std::uint32_t before = generation_;
  now_ms_ = now_ms;
  const bool stopped = playing_ && !playing;  // the published transport fell: a stop (the rise comes a block after the Play key, so a take armed with it never sees a false fall)
  playing_ = playing;
  if (!ready()) {
    return false;
  }
  if (!w_.bank->clear_hook_is(this)) {
    w_.bank->set_clear_hook(&Recorder::on_bank_clear, &Recorder::bank_clear_busy, this);  // (OS-LOOPER; again after a bank that was initialised anew)
  }
  if (loop_review_ && state_ == State::Review) {
    mode = loop_mode_;  // the loop's source is locked while it exists
  }
  if (in_view != in_view_ || mode != mode_) {
    in_view_ = in_view;
    mode_ = mode;
    dirty_ = true;
    touch();
  }
  if (stopped && ((state_ == State::Armed && wait_align_ != RecTap::Align::Level) || (state_ == State::Recording && loop_take_))) {  // (a take that listens for a level has no timeline to lose: it goes on while the transport is stopped)
    stop();  // OS-METRO: a take that waits for its step (aligned, armed under a count-in or a running transport) is given up with the transport, not left to start by itself at a later Play
             // (OS-LOOPER: and a loop take that is recording: its N bars are a count of the transport's steps, which no longer come)
  }
  if (loop_review_ && state_ == State::Review && (w_.bank->take().data != take_ || w_.bank->take().frames == 0u)) {  // a project load emptied the arena under the loop: it is gone
    w_.graph->loop().release();
    loop_review_ = false;
    state_ = State::Idle;
    note_ = Note::Lost;
    give_up_take();
    touch();
  }
  if (state_ == State::Idle && w_.graph->loop().phase() == audio::LoopPlayer::Phase::Idle) {  // OS-BUG-MEMDISCARD: a take thrown away (DISCARD) gives its room back to the arena now, once the render has let go of it
    w_.bank->take_drop(live_idle);  // (no live take in Idle otherwise: a SAVE, a lost take and an empty one have already forgotten theirs)
  }
  if (!loop_review_) {  // no loop under review: no session, no wait
    ov_wanted_ = false;
    ov_seen_ = false;
    rst_seen_ = false;
    ov_refill_ = false;
    save_wait_ = false;
    undo_seq_ = 0;
  } else {
    const audio::LoopPlayer& lp = w_.graph->loop();
    const bool ovb = lp.overdubbing();
    const bool rst = lp.restoring();
    if (ov_wanted_ && !ovb && lp.acked(ov_seq_)) {
      ov_wanted_ = false;  // the render took the command and no session is on (it was refused, or has ended)
    }
    if (ovb != ov_seen_ || rst != rst_seen_) {
      if ((ov_seen_ && !ovb) || (rst_seen_ && !rst)) {  // the loop changed under the overview: it is read again, a bounded amount a poll
        ov_.reset();
        ov_refill_ = true;
      }
      ov_seen_ = ovb;
      rst_seen_ = rst;
      dirty_ = true;
      touch();
    }
    if (ov_refill_ && take_ != nullptr) {
      ov_.feed(take_, frames_, kRefillBudget);
      ov_refill_ = ov_.fed() < frames_;
    }
    if (undo_seq_ != 0u && lp.acked(undo_seq_) && !rst) {
      undo_seq_ = 0;
      touch();
    }
  }
  if (state_ == State::Armed || state_ == State::Recording) {
    RecTap& tap = w_.graph->rec();
    if (count_in_bars_ != 0u) {
      // OS-RECCOUNT: the count-in phase: the tap is not armed yet, so there is no tap state to map; only the arena can take the take away (a project load under the count-in).
      if (!w_.bank->take_held()) {
        give_up_count_in(true);
      }
    } else {
      if (!w_.bank->take_held()) {
        tap.disarm();  // the arena was cleared under the take: end it (the render stops at the next block); finish() sees the hold gone
      }
      const RecTap::State ts = tap.state();
      if (ts == RecTap::State::Done) {
        finish();
      } else {
        const State next = (ts == RecTap::State::Waiting || ts == RecTap::State::Listening) ? State::Armed : State::Recording;
        if (next != state_) {
          state_ = next;
          touch();
        }
        if (ts == RecTap::State::Armed && wait_align_ == RecTap::Align::Level && w_.bank->take_held()) {
          floor_ = tap.settle_ring();  // OS-SREC-LEVEL: the ring is put in time order before the overview reads it (the render writes behind it, never in it); idempotent
        }
        // Fed only for a snapshot of Armed (the tap's state of a take that records): after a Waiting or Listening snapshot the trigger may land before frames() is read, and frames() is then
        // the unordered pre-roll ring, which the overview (not reset before finish()) must not see. The next poll sees Armed, with the ring settled above.
        if (take_ != nullptr && ts == RecTap::State::Armed) {
          const std::uint32_t written = tap.frames();
          ov_.feed(take_, written, kFeedBudget);
          frames_ = written;
          const std::uint32_t t = tenths(written);
          if (t != shown_tenths_) {
            shown_tenths_ = t;
            touch();
          }
        }
      }
    }
  }
  if (ov_.epoch() != view_epoch_) {
    rebuild_view();
    touch();
  }
  if (dirty_ || settling_ || (hw_retry_at_ != 0u && !hw_valid_ && now_ms >= hw_retry_at_)) {
    apply(now_ms, playing);
  }
  if (w_.hw != nullptr) {
    const hal::capture::RadioStatus rs = w_.hw->radio_status();
    if (rs.generation != radio_generation_ || rs.state != radio_.state) {
      radio_generation_ = rs.generation;
      radio_ = rs;
      if (rs.state == hal::capture::RadioStatus::State::Ready) {
        cfg.radio_freq = rs.freq;  // a seek ended on a station: the config follows
      }
      touch();
    }
  }
  return generation_ != before;
}

// ---- Control ----------------------------------------------------------------------------------------------------------------------------

Recorder::ArmResult Recorder::arm(bool playing, std::uint32_t tempo_x100) noexcept {
  return arm_impl(playing, tempo_x100, 0u, 0u, 0u, false);
}

Recorder::ArmResult Recorder::arm_count_in(bool playing, unsigned count_in, unsigned step, unsigned pattern, bool from_stop, std::uint32_t tempo_x100) noexcept {
  return arm_impl(playing, tempo_x100, count_in, step, pattern, from_stop);
}

Recorder::ArmResult Recorder::arm_impl(bool playing, std::uint32_t tempo_x100, unsigned count_in, unsigned step, unsigned pattern, bool from_stop) noexcept {
  if (!ready() || !owned_) {
    return ArmResult::NotReady;
  }
  if (state_ == State::Armed || state_ == State::Recording) {
    return ArmResult::Busy;
  }
  saved_ = false;  // a new take: nothing of it is written out
  const bool looping = cfg.loop_bars != 0u;
  if (looping && !playing) {
    return ArmResult::NotReady;  // the loop is clocked to the bars: the caller starts the transport first
  }
  if (w_.graph->loop().phase() != audio::LoopPlayer::Phase::Idle) {
    return ArmResult::Busy;  // a loop still reads its take (or the render has not let go of a discarded one yet): the memory is not free
  }
  if (w_.graph->scratch().busy()) {
    return ArmResult::Busy;  // OS-DJ C1: the DJ scratch ring is the arena's tail until the render has let go of it (a project load that forgot the held tail must not hand it to a take)
  }
  if (settling_ || dirty_) {
    note_ = Note::Settling;
    touch();
    return ArmResult::Settling;
  }
  if (mode_ != RecMode::Resample && w_.hw != nullptr && !hw_valid_) {
    note_ = Note::CodecFailed;
    touch();
    return ArmResult::NotReady;
  }
  std::uint32_t room = 0;
  std::int16_t* pcm = w_.bank->take_begin(room);
  if (pcm == nullptr) {
    note_ = Note::NoRoom;
    touch();
    return ArmResult::NoRoom;
  }
  // OS-LOOPER L2: a loop take holds its undo buffer and bitmap behind it (SampleBank::take_end's tail), so the free room is split: `cap` frames of loop, `cap` of undo, the bitmap. The render is given `cap`, not the
  // room: a loop that outgrows it overruns and is given up (Note::Full), it never eats its own undo.
  const std::uint32_t cap = looping ? SampleBank::loop_capacity(room) : room;
  if (looping && tempo_x100 != 0u) {  // N bars at this tempo: 16 N steps of rate x 15 / BPM samples (rate in mHz, tempo x 100: x 24 a bar). A tempo that slows down under the take is the arena filling, later.
    const std::uint64_t need = static_cast<std::uint64_t>(hal::audio::sample_rate_mhz()) * 24u * cfg.loop_bars / tempo_x100;
    if (cap < need) {  // (need == cap is a loop; one frame more is not)
      w_.bank->take_end(0);
      note_ = Note::NoRoom;
      touch();
      return ArmResult::NoRoom;
    }
  }
  // OS-LOOPER: a loop take starts on a bar and ends after exactly N bars, whatever ALIGN and STOP AT say; the loop player is told first, so that it is waiting when the take ends.
  const audio::RecTap::Align align = effective_align(looping ? RecAlign::Bar : cfg.align, playing);
  const std::uint32_t auto_steps = looping ? kStepsPerBar * cfg.loop_bars : stop_auto_steps(cfg.stop, cfg.stop_bars, playing);
  if (looping) {
    w_.graph->loop().set_volume(cfg.loop_vol);
    w_.graph->loop().set_feedback(cfg.loop_fb);
    w_.graph->loop().set_muted(false);
    w_.graph->loop().capture(auto_steps);
  }
  if (count_in != 0u) {
    // OS-RECCOUNT: the count-in phase of a bar-aligned take. The arena is held now (the take will use it), but the tap is NOT armed: count_in_poll() arms it on the main loop during the
    // LAST count-in bar, with Align::Bar, so that it picks the boundary after the N bars. From stop the transport is started by the caller (plain play), so `playing` is true here and the
    // phase's first boundary is the transport's start at step 0.
    loop_take_ = looping;
    loop_bars_ = looping ? cfg.loop_bars : 0u;
    loop_tempo_ = looping ? tempo_x100 : 0u;
    loop_mode_ = mode_;
    loop_review_ = false;
    take_ = pcm;
    capacity_ = cap;
    frames_ = 0;
    shown_tenths_ = 0;
    trim_start_ = 0;
    floor_ = 0;
    trim_end_ = 0;
    stop_pending_ = false;
    auto_end_ = auto_steps != 0u;
    ov_.reset();
    wait_align_ = audio::RecTap::Align::Bar;  // the take waits for a bar (the phase counts the bars; the strip's countdown reads count_in_steps_left)
    count_in_bars_ = static_cast<std::uint8_t>(count_in > 0xFFu ? 0xFFu : count_in);
    count_in_seen_ = 0;
    ci_prev_ = from_stop ? kCiNone : step;
    ci_len_ = 0;
    ci_pattern_ = static_cast<std::uint8_t>(pattern);  // the pattern at F1: a later change of it is a switch (counted), not this one
    ci_to_boundary_ = 0xFFFFu;
    ci_arm_tries_ = 0;
    ci_arm_failed_ = false;
    ci_from_stop_ = from_stop;
    state_ = State::Armed;
    note_ = Note::None;
    touch();
    return ArmResult::Ok;
  }
  bool armed = false;
  if (align == audio::RecTap::Align::Level) {
    // OS-SREC-LEVEL: the pre-roll in frames (ms x rate); the region must hold the ring and a second of take behind it, else the arena is too full for a take that means anything.
    const std::uint32_t pre = static_cast<std::uint32_t>(static_cast<std::uint64_t>(hal::audio::sample_rate_mhz()) * kRecPreRollMs[cfg.preroll < kRecPreRolls ? cfg.preroll : kRecPreRollDefault] / 1000000u);
    if (cap < audio::RecTap::ring_frames(pre) + hal::audio::sample_rate_mhz() / 1000u) {
      w_.bank->take_end(0);
      note_ = Note::NoRoom;
      touch();
      return ArmResult::NoRoom;
    }
    armed = w_.graph->rec().arm_level(pcm, cap, 1, rec_level_threshold(cfg.level_db), pre, auto_steps);
  } else {
    armed = w_.graph->rec().arm(pcm, cap, 1, align, auto_steps);
  }
  if (!armed) {
    if (looping) {
      w_.graph->loop().release();
    }
    w_.bank->take_end(0);
    return ArmResult::Busy;
  }
  loop_take_ = looping;
  loop_bars_ = looping ? cfg.loop_bars : 0u;
  loop_tempo_ = looping ? tempo_x100 : 0u;
  loop_mode_ = mode_;
  loop_review_ = false;
  take_ = pcm;
  capacity_ = cap;
  frames_ = 0;
  shown_tenths_ = 0;
  trim_start_ = 0;
  floor_ = 0;
  trim_end_ = 0;
  stop_pending_ = false;
  auto_end_ = auto_steps != 0u;
  ov_.reset();
  wait_align_ = align;
  const RecTap::State first = w_.graph->rec().state();
  state_ = (first == RecTap::State::Waiting || first == RecTap::State::Listening) ? State::Armed : State::Recording;
  note_ = Note::None;
  touch();
  return ArmResult::Ok;
}

// OS-RECCOUNT: one pass of the count-in phase of the armed take, from the published position: `pattern` and `step` of the pattern `pattern_len` steps long now running (pattern_len 0:
// no position, the phase does not advance), `playing` the transport. Main loop only. The bar boundaries that have come since the phase began are counted (C_0, C_1, ...; the take starts
// at C_N, N = count_in_bars_), and during the LAST count-in bar the tap is armed with Align::Bar, so that it picks C_N. A bar lasts at least 300 ms at the top tempo (800 BPM) and a pass is
// 16 ms, so the pass that sees the last count-in bar begin has a whole bar of margin.
void Recorder::count_in_poll(unsigned pattern, unsigned step, unsigned pattern_len, bool playing) noexcept {
  if (state_ != State::Armed || count_in_bars_ == 0u || w_.graph == nullptr || pattern_len == 0u) {
    return;
  }
  if (!playing) {
    ci_prev_ = kCiNone;  // (the stop gives the take up in poll(); the reference is reset in case the phase survives it, which it does not)
    return;
  }
  if (ci_prev_ == kCiNone) {
    if (ci_from_stop_) {
      count_in_seen_ = static_cast<std::uint8_t>(step / kStepsPerBar + 1u);  // the transport started at step 0: that boundary is C_0
      ci_from_stop_ = false;
    }
    ci_prev_ = step;
    ci_len_ = pattern_len;
    ci_pattern_ = static_cast<std::uint8_t>(pattern);
  } else if (pattern != ci_pattern_) {
    // Another pattern is running (a Song slot change, a pattern switch): its step 0 is a bar boundary the tap accepts and it was never counted, so count it now, with every
    // boundary of the new pattern the position has already passed (on the device the position shows the new pattern only from the block its step 0 fired in).
    count_in_seen_ = static_cast<std::uint8_t>(count_in_seen_ + step / kStepsPerBar + 1u);
    ci_prev_ = step;
    ci_len_ = pattern_len;
    ci_pattern_ = static_cast<std::uint8_t>(pattern);
  } else if (pattern_len == ci_len_) {
    if (step > ci_prev_) {
      count_in_seen_ = static_cast<std::uint8_t>(count_in_seen_ + (step / kStepsPerBar - ci_prev_ / kStepsPerBar));
    } else if (step < ci_prev_) {  // the pattern wrapped: the boundaries in (ci_prev_, len-1] and in [0, step]
      count_in_seen_ = static_cast<std::uint8_t>(count_in_seen_ + ((ci_len_ - 1u) / kStepsPerBar - ci_prev_ / kStepsPerBar) + (step / kStepsPerBar + 1u));
    }
    ci_prev_ = step;
  } else {  // the same pattern, another length (edited under the count-in): the reference moves, nothing is counted
    ci_prev_ = step;
    ci_len_ = pattern_len;
  }
  const unsigned to_boundary = steps_to_bar_boundary(step, pattern_len);
  // Arm as soon as the count has reached N (the last count-in bar is running). The "not within the last step before the boundary" guard applies only when this pass is what moved
  // into that last step (a delayed pass: the boundary may be imminent and the arm could reach the render after its Step event); a last count-in bar of a single step (a 17/33/49-step
  // pattern) would never pass that guard, so once the count is reached the take arms whatever the distance.
  const bool into_last_step = to_boundary < 2u && ci_to_boundary_ >= 2u;
  ci_to_boundary_ = to_boundary;
  if (count_in_seen_ >= count_in_bars_ && !into_last_step) {
    // (seen > N: whole boundaries passed within one pass — impossible at the top tempo, a bar is >= 300 ms against a 16 ms pass — or the guard skipped a pass; arm at once anyway,
    // the tap then picks the next boundary, which is the best that is left.)
    const std::uint32_t auto_steps = loop_take_ ? kStepsPerBar * loop_bars_ : stop_auto_steps(cfg.stop, cfg.stop_bars, true);
    if (w_.graph->rec().arm(take_, capacity_, 1, audio::RecTap::Align::Bar, auto_steps)) {
      count_in_bars_ = 0;  // the phase is over: from here on the take is a plain armed bar-aligned one (poll() maps the tap's Waiting, the strip counts the last bar as a plain wait)
      ci_arm_tries_ = 0;
      touch();
    } else if (++ci_arm_tries_ >= kCiArmMaxTries) {
      // The tap refuses the arm (it is not Idle): retry a bounded number of passes, then give the count-in up as a Stop during it does, and let the App say so once.
      ci_arm_failed_ = true;
      give_up_count_in(false);
    }
  }
}

// OS-RECCOUNT: the steps left until the boundary the count-in take starts at, from `step` of a pattern `pattern_len` steps long: to the next boundary, plus a bar per count-in bar still to
// come (the strip's countdown, as wait_steps_left is for the plain wait).
unsigned Recorder::count_in_steps_left(unsigned step, unsigned pattern_len) const noexcept {
  const unsigned seen = count_in_seen_ < count_in_bars_ ? count_in_seen_ : count_in_bars_;
  return steps_to_bar_boundary(step, pattern_len) + kStepsPerBar * (count_in_bars_ - seen);
}

// OS-RECCOUNT: the count-in phase ends without a take (a Stop or F1 during it, or the arena cleared under it): the tap was never armed, so there is nothing for the render to finish. The
// hold goes back at once and the state is Idle with no take (Note::Lost when a project load took the arena).
void Recorder::give_up_count_in(bool lost) noexcept {
  if (w_.graph != nullptr) {
    w_.graph->rec().disarm();  // (harmless on an Idle tap: arm_as() resets the flag)
    if (loop_take_) {
      w_.graph->loop().release();
    }
  }
  count_in_bars_ = 0;
  count_in_seen_ = 0;
  ci_prev_ = kCiNone;
  ci_len_ = 0;
  ci_pattern_ = 0;
  ci_to_boundary_ = 0xFFFFu;
  ci_arm_tries_ = 0;
  // (ci_arm_failed_ is not cleared here: the App reads it once to say the count-in did not start)
  ci_from_stop_ = false;
  loop_take_ = false;
  w_.bank->take_end(0);
  stop_pending_ = false;
  state_ = State::Idle;
  note_ = lost ? Note::Lost : Note::None;
  give_up_take();
  touch();
}

void Recorder::stop() noexcept {
  if ((state_ == State::Armed || state_ == State::Recording) && w_.graph != nullptr) {
    if (count_in_bars_ != 0u) {  // OS-RECCOUNT: the count-in phase: the tap is not armed yet, the take is given up at once (as a waiting take is, at the next block)
      give_up_count_in(false);
      return;
    }
    audio::RecTap& tap = w_.graph->rec();
    const audio::RecTap::Align boundary = stop_boundary(effective_stop(cfg.stop, playing_));
    if (loop_take_ || boundary == audio::RecTap::Align::Free || stop_pending_ || tap.state() != audio::RecTap::State::Armed) {  // (a loop take's early end is a cancel, at the next block)
      tap.disarm();  // Manual (and a stopped transport, and Bars), a second press, a take that has not started: at the next block, as before OS-REC2
    } else {
      tap.stop_at(boundary);  // the render ends the take at the sample of the boundary (or where the transport stops)
      stop_pending_ = true;
      touch();
    }
  }
}

void Recorder::discard() noexcept {
  if (state_ == State::Review) {
    if (loop_review_) {  // the loop fades out within a block and the render lets go of the take (arm() waits for that); the memory is the next take's, as any discarded take's
      w_.graph->loop().release();  // (a session or a restore in progress is abandoned with it: the render writes nothing in the block that releases)
      loop_review_ = false;
      ov_wanted_ = false;
      save_wait_ = false;
    }
    state_ = State::Idle;
    give_up_take();
    touch();
  }
}

bool Recorder::commit(unsigned slot) noexcept {
  save_wait_ = false;
  if (state_ != State::Review || w_.bank == nullptr || trim_end_ <= trim_start_) {
    return false;
  }
  if (!w_.bank->take_commit(slot, trim_start_, trim_end_ - trim_start_)) {
    return false;
  }
  if (loop_review_) {  // the slot owns the take's memory now (read only, as the loop reads it): the loop fades out and lets go, the looper is free for the next take
    w_.graph->loop().release();
    loop_review_ = false;
  }
  state_ = State::Idle;
  give_up_take();
  touch();
  return true;
}

void Recorder::step_source(int detents) noexcept {
  const int next = clamp_int(static_cast<int>(cfg.source) + detents, 0, static_cast<int>(hal::capture::kSourceCount) - 1);
  if (next != static_cast<int>(cfg.source)) {
    cfg.source = static_cast<hal::capture::Source>(next);
    dirty_ = true;
    touch();
  }
}

void Recorder::step_gain(int detents) noexcept {
  const std::uint8_t bank = source_info(source()).gain_bank;
  if (bank == kNoGain) {
    return;
  }
  const int next = clamp_int(static_cast<int>(cfg.gain[bank]) + detents, 0, 100);
  if (next != static_cast<int>(cfg.gain[bank])) {
    cfg.gain[bank] = static_cast<std::uint8_t>(next);
    dirty_ = true;
    touch();
  }
}

void Recorder::reset_gain() noexcept {
  const std::uint8_t bank = source_info(source()).gain_bank;
  if (bank != kNoGain && cfg.gain[bank] != 0u) {
    cfg.gain[bank] = 0;  // our default
    dirty_ = true;
    touch();
  }
}

void Recorder::step_align(int detents) noexcept {
  const int next = clamp_int(static_cast<int>(cfg.align) + detents, 0, static_cast<int>(kRecAligns) - 1);
  if (next != static_cast<int>(cfg.align)) {
    cfg.align = static_cast<RecAlign>(next);
    touch();
  }
}

void Recorder::step_threshold(int detents) noexcept {
  const int next = clamp_int(cfg.level_db + detents * kRecLevelStepDb, kRecLevelMinDb, kRecLevelMaxDb);
  if (next != cfg.level_db) {
    cfg.level_db = static_cast<std::int8_t>(next);
    touch();
  }
}

void Recorder::step_preroll(int detents) noexcept {
  const int next = clamp_int(static_cast<int>(cfg.preroll) + detents, 0, static_cast<int>(kRecPreRolls) - 1);
  if (next != static_cast<int>(cfg.preroll)) {
    cfg.preroll = static_cast<std::uint8_t>(next);
    touch();
  }
}

void Recorder::step_stop(int detents) noexcept {
  const int cur = cfg.stop == RecStop::Bars ? 3 + clamp_int(cfg.stop_bars, 1, static_cast<int>(kRecMaxBars)) : static_cast<int>(cfg.stop);
  const int i = clamp_int(cur + detents, 0, static_cast<int>(kRecStopChoices) - 1);
  if (i == cur) {
    return;
  }
  cfg.stop = i < 4 ? static_cast<RecStop>(i) : RecStop::Bars;
  cfg.stop_bars = i < 4 ? std::uint8_t{1} : static_cast<std::uint8_t>(i - 3);
  touch();
}

void Recorder::step_loop(int detents) noexcept {
  const int next = clamp_int(static_cast<int>(cfg.loop_bars) + detents, 0, static_cast<int>(kRecMaxBars));
  if (next != static_cast<int>(cfg.loop_bars)) {
    cfg.loop_bars = static_cast<std::uint8_t>(next);
    touch();
  }
}

// OS-RECCOUNT: the COUNT-IN row steps through OFF, 1 BAR, 2 BARS, 4 BARS (no wrap at either end; a detent is one choice, however many it turns).
void Recorder::step_count_in(int detents) noexcept {
  const int next = clamp_int(static_cast<int>(rec_count_in_index(cfg.count_in)) + detents, 0, static_cast<int>(kRecCountInChoices) - 1);
  const std::uint8_t v = kRecCountInBars[static_cast<unsigned>(next)];
  if (v != cfg.count_in) {
    cfg.count_in = v;
    touch();
  }
}

void Recorder::step_loop_vol(int detents) noexcept {
  const int next = clamp_int(static_cast<int>(cfg.loop_vol) + detents, 0, 100);
  if (next != static_cast<int>(cfg.loop_vol)) {
    cfg.loop_vol = static_cast<std::uint8_t>(next);
    if (w_.graph != nullptr) {
      w_.graph->loop().set_volume(cfg.loop_vol);  // heard from the next block
    }
    touch();
  }
}

void Recorder::toggle_loop_mute() noexcept {
  if (w_.graph != nullptr && loop_review_) {
    w_.graph->loop().set_muted(!w_.graph->loop().muted());
    touch();
  }
}

void Recorder::step_loop_fb(int detents) noexcept {
  const int next = clamp_int(static_cast<int>(cfg.loop_fb) + detents, 0, 100);
  if (next != static_cast<int>(cfg.loop_fb)) {
    cfg.loop_fb = static_cast<std::uint8_t>(next);
    if (w_.graph != nullptr) {
      w_.graph->loop().set_feedback(cfg.loop_fb);  // heard from the next block
    }
    touch();
  }
}

bool Recorder::needs_quiesce() const noexcept {
  return loop_review_ && w_.graph != nullptr && (ov_wanted_ || w_.graph->loop().overdubbing() || !w_.graph->loop().acked(ov_seq_));
}

void Recorder::begin_save() noexcept {
  audio::LoopPlayer& lp = w_.graph->loop();
  ov_wanted_ = false;
  save_seq_ = lp.quiesce();
  ov_seq_ = save_seq_;
  save_wait_ = true;
  save_t0_ = now_ms_;
  touch();
}

Recorder::Overdub Recorder::overdub_toggle() noexcept {
  if (!loop_review_ || state_ != State::Review || w_.graph == nullptr || take_ == nullptr) {
    return Overdub::NoLoop;
  }
  if (save_wait_) {
    return Overdub::Ignored;
  }
  audio::LoopPlayer& lp = w_.graph->loop();
  if (undo_busy()) {
    return Overdub::Undoing;
  }
  if (ov_wanted_ || lp.overdubbing()) {  // punch out
    ov_wanted_ = false;
    ov_seq_ = lp.quiesce();
    touch();
    return Overdub::Stopped;
  }
  if (!lp.acked(ov_seq_)) {
    return Overdub::Ignored;  // the last command (a punch-out) has not been acknowledged: the bitmap may still be in use
  }
  // The bitmap is cleared here, by the main loop: the render touches it only while a session is on or a restore runs, and neither is (checked above).
  const std::uint32_t even = (frames_ + 1u) & ~1u;
  std::memset(take_ + 2u * even, 0, SampleBank::undo_bitmap_samples(frames_) * sizeof(std::int16_t));
  lp.set_feedback(cfg.loop_fb);
  ov_wanted_ = true;
  ov_seq_ = lp.overdub_on();
  dirty_ = true;  // the session owns the input path
  touch();
  return Overdub::Started;
}

bool Recorder::undo_layer() noexcept {
  if (!loop_review_ || state_ != State::Review || w_.graph == nullptr || save_wait_ || undo_busy() || !w_.graph->loop().layer()) {
    return false;
  }
  ov_wanted_ = false;
  undo_seq_ = w_.graph->loop().undo();
  ov_seq_ = undo_seq_;
  touch();
  return true;
}

void Recorder::step_tap(int detents) noexcept {
  const int i = clamp_int(static_cast<int>(tap_index(cfg)) + detents, 0, static_cast<int>(kTapChoices) - 1);
  if (i == static_cast<int>(tap_index(cfg))) {
    return;
  }
  cfg.tap = i == 0 ? RecTapKind::Master : (i == 1 ? RecTapKind::RecBus : RecTapKind::Track);
  cfg.track = i >= 2 ? static_cast<std::uint8_t>(i - 2) : 0u;
  dirty_ = true;
  touch();
}

void Recorder::radio_step(int detents) noexcept {
  const unsigned f = cap::step_freq(cfg.region, cfg.radio_freq, detents);
  if (f == cfg.radio_freq) {
    return;
  }
  cfg.radio_freq = static_cast<std::uint16_t>(f);
  if (w_.hw != nullptr) {
    w_.hw->radio_tune(cfg.radio_freq);
  }
  touch();
}

void Recorder::radio_set(std::uint16_t freq) noexcept {
  const std::uint16_t f = static_cast<std::uint16_t>(cap::snap_freq(cfg.region, freq));
  if (f == cfg.radio_freq) {
    return;
  }
  cfg.radio_freq = f;
  if (w_.hw != nullptr) {
    w_.hw->radio_tune(f);
  }
  touch();
}

void Recorder::radio_seek(bool up) noexcept {
  if (w_.hw != nullptr) {
    w_.hw->radio_seek(up);
  }
}

void Recorder::radio_region_step(int detents) noexcept {
  const int next = clamp_int(static_cast<int>(cfg.region) + detents, 0, static_cast<int>(cap::kRegionCount) - 1);
  if (next == static_cast<int>(cfg.region)) {
    return;
  }
  cfg.region = static_cast<cap::Region>(next);
  cfg.radio_freq = static_cast<std::uint16_t>(cap::snap_freq(cfg.region, cfg.radio_freq));
  if (w_.hw != nullptr) {
    w_.hw->radio_region(cfg.region);
  }
  touch();
}

void Recorder::set_trim_start(std::uint32_t frame) noexcept {
  if (state_ != State::Review) {
    return;
  }
  const std::uint32_t max = trim_end_ > kMinWindow ? trim_end_ - kMinWindow : 0u;
  const std::uint32_t v0 = frame < max ? frame : max;
  const std::uint32_t v = v0 < floor_ ? floor_ : v0;  // (OS-SREC-LEVEL: the frames before the floor are no audio)
  if (v != trim_start_) {
    trim_start_ = v;
    saved_ = false;
    touch();
  }
}

void Recorder::set_trim_end(std::uint32_t frame) noexcept {
  if (state_ != State::Review) {
    return;
  }
  const std::uint32_t min = trim_start_ + kMinWindow < frames_ ? trim_start_ + kMinWindow : frames_;
  const std::uint32_t v = frame < min ? min : (frame > frames_ ? frames_ : frame);
  if (v != trim_end_) {
    trim_end_ = v;
    saved_ = false;
    touch();
  }
}

// OS-RECCROP: the take becomes its window. The bank moves the audio down to the take's start and gives the room behind it back (SampleBank::take_crop); the markers are the whole of the shorter take
// again, and the overview is drawn over it. A window that starts on an odd frame keeps that one frame (the bank's alignment). A loop's take is not cropped (the loop player reads it).
bool Recorder::crop() noexcept {
  if (state_ != State::Review || loop_review_ || w_.bank == nullptr || trim_end_ <= trim_start_ || (trim_start_ == 0u && trim_end_ == frames_)) {
    return false;
  }
  const std::uint32_t at = trim_start_ & ~1u;
  const std::uint32_t len = trim_end_ - at;
  if (!w_.bank->take_crop(at, len)) {
    return false;
  }
  frames_ = len;
  trim_start_ -= at;
  floor_ = 0;  // (the window starts on or after the floor: the zeros are gone)
  trim_end_ = len;
  saved_ = false;
  ov_.reset();
  ov_.feed(take_, frames_, 0xFFFFFFFFu);
  touch();
  return true;
}

void Recorder::step_trim_start(int detents, std::uint32_t unit) noexcept {
  const std::int64_t v = static_cast<std::int64_t>(trim_start_) + static_cast<std::int64_t>(detents) * unit;
  set_trim_start(v < 0 ? 0u : (v > 0xFFFFFFFFll ? 0xFFFFFFFFu : static_cast<std::uint32_t>(v)));
}

void Recorder::step_trim_end(int detents, std::uint32_t unit) noexcept {
  const std::int64_t v = static_cast<std::int64_t>(trim_end_) + static_cast<std::int64_t>(detents) * unit;
  set_trim_end(v < 0 ? 0u : (v > 0xFFFFFFFFll ? 0xFFFFFFFFu : static_cast<std::uint32_t>(v)));
}

}  // namespace o3os::app
