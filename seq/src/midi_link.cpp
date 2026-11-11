// SPDX-License-Identifier: GPL-3.0-or-later
//
// MIDI sync end to end, the render side (seq/midi_link.h).
#include <o3os/seq/midi_link.h>

#include <o3os/core/audio_clock.h>
#include <o3os/model/params.h>

namespace o3os::seq {

using audio::EventKind;
using midi::MidiConfig;
using midi::SyncMode;

namespace {
// OS-MTC B3: the clock is followed only when CLOCK IN / CLOCK MODE say Slave and MTC IN is Off (the chase of the time code replaces the follow). A Slave with MTC IN on acts as a Master for the clock out.
constexpr bool slave_of(const MidiConfig& c) noexcept { return c.mode == SyncMode::Slave && c.mtc_in == 0u; }
constexpr bool master_of(const MidiConfig& c) noexcept { return c.mode == SyncMode::Master || (c.mode == SyncMode::Slave && c.mtc_in != 0u); }

constexpr std::uint32_t kChaseMinLead = 256;       // samples: the start must be this far ahead of the block that gives it (the block, the run's Start / SPP lead and room)
constexpr std::uint32_t kChaseSlewQ16 = 2048;      // the rate trim moves at most 3 % a step
constexpr std::uint32_t kChaseGoodUs = 1000;       // |e| below 1 ms is in phase
constexpr std::uint32_t kChaseSettleWindow = 4410;  // samples (100 ms, about ten quarter frames at 25 fps) the start sample must hold still over before the run begins
constexpr std::uint32_t kChaseSettleFrames = 160;   // quarter frames read since a relocation: only then is the fit (and its rate) converged to a sample
constexpr std::uint32_t kChaseSettleMax = 88200;    // samples (2 s): a start that never settles begins anyway with the latest mapping
constexpr std::uint64_t kChaseSettleTol = 44;       // samples of movement allowed over the window (about 1 ms: a master with stamp jitter still settles)
}  // namespace

MidiLink::MidiLink(Sequencer& sequencer, const MidiLinkHooks& hooks) noexcept : seq_(sequencer), hooks_(hooks) {
  out_.attach(&din_, &usb_out_);
  apply_config(cfg_, 0);  // the defaults, into the router and the sequencer's clock-out switch
  stats_.configs = 0;
}

void MidiLink::set_config(const MidiConfig& c) noexcept { box_.write(midi::sanitized(c)); }

midi::SyncStatus MidiLink::status() const noexcept {
  midi::SyncStatus s;
  const std::uint32_t f = st_flags_.load(std::memory_order_relaxed);
  s.following = (f & 1u) != 0u;
  s.clock_present = (f & 2u) != 0u;
  s.locked = (f & 4u) != 0u;
  s.bpm_x100 = st_bpm_.load(std::memory_order_relaxed);
  s.clocks = st_clocks_.load(std::memory_order_relaxed);
  s.error_us = st_error_us_.load(std::memory_order_relaxed);
  s.transport_events = st_transport_.load(std::memory_order_relaxed);
  const std::uint32_t m = st_mtc_.load(std::memory_order_relaxed);
  s.mtc_state = static_cast<std::uint8_t>(m & 0xFFu);
  s.mtc_code = static_cast<std::uint8_t>((m >> 8) & 0xFFu);
  s.mtc_time = st_mtc_time_.load(std::memory_order_relaxed);
  s.mtc_rate_ppm = st_mtc_ppm_.load(std::memory_order_relaxed);
  s.mtc_error_us = st_mtc_err_us_.load(std::memory_order_relaxed);
  s.mtc_relocations = st_mtc_rel_.load(std::memory_order_relaxed);
  return s;
}

audio::EventSource MidiLink::event_source(audio::SeqEvent* buffer, std::uint32_t capacity) noexcept {
  audio::EventSource source;
  source.self = this;
  source.step = [](void* self, std::uint64_t block_start, std::uint32_t n, audio::SeqEvent* out, std::uint32_t cap) noexcept -> std::uint32_t {
    return static_cast<MidiLink*>(self)->step(block_start, n, out, cap);
  };
  source.param = [](void* self, const core::ParamChange& change) noexcept -> bool { return static_cast<MidiLink*>(self)->seq_.apply(change); };
  source.tempo = [](void* self) noexcept -> std::uint32_t { return static_cast<MidiLink*>(self)->seq_.tempo_x100(); };
  source.clock = [](void* self, std::uint64_t sample) noexcept -> audio::ClockPos { return static_cast<MidiLink*>(self)->seq_.clock_at(sample); };  // OS-INSFX-ABI2 A3: get_clock; the unit's source is this one
  source.live = [](void* self, const audio::LiveEvent& e) noexcept -> bool { return static_cast<MidiLink*>(self)->live_event(e); };
  source.resume = [](void* self) noexcept { static_cast<MidiLink*>(self)->resume(); };
  source.stop_live = [](void* self, std::uint32_t* count) noexcept -> std::uint32_t { return static_cast<MidiLink*>(self)->stop_live_pads(count); };
  source.buffer = buffer;
  source.capacity = capacity;
  return source;
}

// ---- the block ----------------------------------------------------------------------------------------------------------------

std::uint32_t MidiLink::step(std::uint64_t block_start, std::uint32_t n, audio::SeqEvent* out, std::uint32_t cap) noexcept {
  // Without a platform hook the counter is taken to read the timeline's own cycles (the host tests drive the emitter with timeline times directly).
  const std::uint32_t heard = hooks_.heard_at != nullptr ? hooks_.heard_at(hooks_.ctx) : static_cast<std::uint32_t>(core::cycles_from_samples(block_start));
  begin_block(block_start, heard);

  const std::uint32_t count = seq_.step(block_start, n, out, cap);
  for (std::uint32_t i = 0; i < count; ++i) {
    const audio::SeqEvent& e = out[i];
    if (e.kind >= EventKind::MidiNoteOn && e.kind <= EventKind::MidiMtcFull) {  // (OS-MTC A3: 28 and 29 are the time code)
      (void)out_.send(e, block_start);
    } else if (e.kind == EventKind::Stopped) {
      // The sequencer has sent the offs of the notes it knew about (they came before this event); what is still owed after them is a note it lost track of.
      out_.stop_all(core::cycles_from_samples(block_start + e.offset));
    }
  }
  last_block_end_ = block_start + n;
  out_.flush(core::cycles_from_samples(last_block_end_));

  const std::uint32_t handed = out_.stats().handed;
  if (handed != handed_) {
    handed_ = handed;
    if (hooks_.kick != nullptr) {
      hooks_.kick(hooks_.ctx);  // the emitter's interrupt decides from its queues: a pend is always safe
      ++stats_.kicks;
    }
  }
  ++stats_.blocks;
  publish_status(block_start);
  return count;
}

// ---- live MIDI notes (the pads on a MIDI instrument; seq/midi_link.h LIVE MIDI NOTES) -----------------------------------------------

void MidiLink::live_release(std::uint32_t pad) noexcept {
  const std::uint32_t held = live_pad_[pad];
  if ((held & kLiveHeld) == 0u) {
    return;
  }
  live_pad_[pad] = 0;
  audio::SeqEvent ev{};
  ev.kind = EventKind::MidiNoteOff;
  ev.track = audio::kNoTrack;
  ev.a = static_cast<std::uint8_t>(held & 0x7Fu);
  ev.c = static_cast<std::uint8_t>((held >> 8) & 0x0Fu);
  ev.flags = static_cast<std::uint8_t>((held >> kLivePortsShift) & audio::kRoutePorts);  // the route of the note-on (OS-INSTUI S8b): an OUT, CHANNEL or OFFSET edit under the held pad changes only the next note
  ev.value = static_cast<std::int32_t>((held >> kLiveDelayShift) & 0x7Fu) * 1000;
  (void)out_.send(ev, last_block_end_);  // an off that is not owed (a stop took the note) is dropped by the router
}

std::uint32_t MidiLink::stop_live_pads(std::uint32_t* count) noexcept {
  std::uint32_t word = audio::kHeldNone;
  std::uint32_t n = 0;
  for (std::uint32_t pad = 0; pad < kLivePads; ++pad) {
    const std::uint32_t held = live_pad_[pad];
    if ((held & kLiveHeld) == 0u) {
      continue;
    }
    if (n == 0u) {
      word = static_cast<std::uint32_t>(audio::kLiveSourcePad) | (pad << 8) | (static_cast<std::uint32_t>(held & 0x7Fu) << 16) | ((((held >> 8) & 0x0Fu) + 1u) << 24);
    }
    ++n;
    live_release(pad);
  }
  if (n != 0u) {
    out_.flush(core::cycles_from_samples(last_block_end_));  // as step() ends: the offs go to the ports now
    if (hooks_.kick != nullptr) {
      hooks_.kick(hooks_.ctx);
      ++stats_.kicks;
    }
  }
  *count = n;
  return word;
}

bool MidiLink::live_event(const audio::LiveEvent& e) noexcept {
  if (e.kind == audio::LiveKind::On) {
    if (e.source != audio::kLiveSourcePad || e.key >= kLivePads) {
      return false;
    }
    live_release(e.key);  // a pad that goes down while it still holds a note (a lost release) ends that note first
    const model::Instrument* in = e.instrument >= 1u ? seq_.project().instrument(static_cast<std::uint32_t>(e.instrument) - 1u) : nullptr;
    if (in == nullptr || in->kind != model::InstrumentKind::Midi) {
      return false;
    }
    const MidiRoute r = midi_route(*in);  // as Sequencer::send_note_on: the channel 1..16 -> 0..15, the port and the OFFSET of the instrument (OS-INSTUI S8b)
    const std::uint8_t ch = r.ch;
    const std::uint8_t note = e.note > 127u ? std::uint8_t{127} : e.note;
    const std::uint8_t vel = e.velocity == audio::kLiveVelocityDefault ? midi_velocity_of(*in) : (e.velocity == 0u ? std::uint8_t{1} : (e.velocity > 127u ? std::uint8_t{127} : e.velocity));
    audio::SeqEvent ev{};
    ev.track = audio::kNoTrack;
    ev.c = ch;
    ev.flags = r.ports;
    ev.value = route_delay_us(r);
    const int program = seq_.program_due(*in);  // the receiver is put on the instrument's program before its first note
    if (program >= 0) {
      ev.kind = EventKind::MidiProgram;
      ev.a = static_cast<std::uint8_t>(program);
      (void)out_.send(ev, last_block_end_);
      seq_.program_sent(*in, program);  // limit of the pad path: MidiOut::send does not report a full staging queue, so the program is taken as told once it was handed over
    }
    ev.kind = EventKind::MidiNoteOn;
    ev.a = note;
    ev.b = vel;
    (void)out_.send(ev, last_block_end_);
    live_pad_[e.key] = kLiveHeld | (static_cast<std::uint32_t>(r.ports) << kLivePortsShift) | (static_cast<std::uint32_t>(r.delay_ms) << kLiveDelayShift) | (static_cast<std::uint32_t>(ch) << 8) | note;
    return true;
  }
  if (e.kind == audio::LiveKind::Off) {
    if (e.source == audio::kLiveSourcePad && e.key < kLivePads) {
      live_release(e.key);
    }
  } else if (e.kind == audio::LiveKind::AllOff && (e.source == audio::kLiveSourcePad || e.source == audio::kLiveAllSources)) {
    for (std::uint32_t pad = 0; pad < kLivePads; ++pad) {
      live_release(pad);
    }
  }
  return false;
}

void MidiLink::begin_block(std::uint64_t block_start, std::uint32_t heard) noexcept {
  ref_.publish(block_start, heard);   // the interrupts' time base
  map_.set_ref(block_start, heard);   // the stamps of incoming bytes
  MidiConfig c;
  if (box_.read_if_changed(c, box_seen_)) {
    apply_config(c, block_start);
  }
  drain_inputs();
  manage_follow(block_start);
  manage_chase(block_start);
}

// ---- configuration ------------------------------------------------------------------------------------------------------------

void MidiLink::apply_config(const MidiConfig& c, std::uint64_t block_start) noexcept {
  const MidiConfig old = cfg_;
  cfg_ = c;
  ++stats_.configs;
  const bool was_slave = slave_of(old);
  const bool is_slave = slave_of(c);
  const bool source_changed = was_slave != is_slave || (is_slave && old.in_port != c.in_port);
  const bool mtc_on = c.mtc_in != 0u;

  // The router: where notes and clock go, the CC table, the OUT OFFSETs.
  midi::MidiOut::Config oc = out_.config();
  oc.note_ports = c.note_ports;
  oc.clock_ports = c.mode == SyncMode::Off ? std::uint8_t{0} : c.out_ports;
  oc.mtc_ports = (c.mtc_fps != 0u && !mtc_on) ? c.out_ports : std::uint8_t{0};  // OS-MTC A3 (A-1): the time code goes where CLOCK OUT says, with any CLOCK MODE; B3 (4.5): MTC IN not Off mutes it (no echo between two devices)
  oc.cc_channel = c.notes_channel;
  for (std::uint32_t i = 0; i < 6u; ++i) {
    oc.cc_number[i] = c.cc_number[i];
  }
  for (std::uint32_t p = 0; p < midi::kPortCount; ++p) {
    oc.offset_cycles[p] = midi::offset_cycles(c.out_offset_dms[p]);
  }
  out_.configure(oc);

  // The sequencer's clock-out switch: on while there is a port to send to (Master: the tracker's clock; Slave: the relay). With clock out on as a Master a Play begins one
  // block late so that SPP and Start go first. Both take effect at the next Play (the run's own clock state is set when it begins).
  Sequencer::Config sc = seq_.config();
  sc.emit_clock = oc.clock_ports != 0u;
  sc.emit_mtc = c.mtc_fps != 0u && oc.mtc_ports != 0u;  // OS-MTC A3: the generator runs when MTC OUT is on and a port takes it
  sc.mtc_fps = c.mtc_fps;
  sc.mtc_hour = c.mtc_hour;
  sc.emit_transport = c.transport_out;  // E-CFG-03: read by the sequencer when a run begins (render thread: this runs at a block start)
  sc.start_lead = ((master_of(c) && sc.emit_clock && c.transport_out) || sc.emit_mtc) ? kStartLead : 0u;  // OS-MTC A3 (A-8): with MTC OUT on a Play also begins one block late, so the full frame is due before the first step
  seq_.configure(sc);

  // MIDI in: configure() forgets an armed Start, so it is called only when something it holds changed.
  midi::MidiIn::Config ic = in_.config();
  if (ic.clock_enabled != is_slave || ic.clock_port != c.in_port || ic.start_pulses != c.sync_correction) {
    ic.clock_enabled = is_slave;
    ic.clock_port = c.in_port;
    ic.start_pulses = c.sync_correction;
    in_.configure(ic);
  }
  // OS-MTC B3: the time code of MTC IN's port goes to the chaser; a change of the switch or the port starts the chase over (the run that was the master's stops).
  const bool mtc_changed = old.mtc_in != c.mtc_in;
  if (ic.mtc_enabled != mtc_on || (mtc_on && ic.mtc_port != (c.mtc_in == 1u ? midi::Port::Din : midi::Port::Usb))) {
    ic.mtc_enabled = mtc_on;
    ic.mtc_port = c.mtc_in == 1u ? midi::Port::Din : midi::Port::Usb;
    in_.configure(ic);
  }
  if (mtc_changed) {
    in_.reset();  // a half message of the other port's parser must not become a quarter frame
    chase_reset();
  }
  midi::ClockDll::Config dc = dll_.config();
  dc.track_mhz = midi::follow_mhz(c.follow);
  dll_.configure(dc);

  if (source_changed) {
    // Another timeline source: what the rings hold is the old one's (or from before anybody looked), the parsers' state belongs to it, and the loop starts over. A sequencer that
    // follows lets go (stopping first) in manage_follow().
    discard_inputs();
    in_.reset();
    reset_loop(block_start);
    if (seq_.following()) {
      want_release_ = true;
    }
  }
  if (seq_.following() && !want_release_) {
    seq_.set_follow(&dll_, in_offset_us());  // a new IN OFFSET takes effect at once
  }
}

// OS-WAVEXP: the engine was the export's for a while and this link was not stepped. The interrupts went on stamping bytes into the rings (their stamps are cycle counts of a time that is
// 10 or 20 seconds gone: they map to nonsense, and a stale Start, Continue or SPP would start the transport as the export ends), the loop's phase is as old, and the parsers hold a half message:
// the same start over as a change of the followed source (apply_config). Main loop, the render parked: nothing else touches the rings' consumer side.
void MidiLink::resume() noexcept {
  discard_inputs();
  in_.reset();
  chase_reset();
  reset_loop(last_block_end_);
  if (seq_.following()) {
    want_release_ = true;
  }
}

void MidiLink::reset_loop(std::uint64_t origin) noexcept {
  dll_.reset(origin, 8u * core::samples_per_tick_q32(seq_.nominal_tempo_x100()));
}

// ---- input --------------------------------------------------------------------------------------------------------------------

void MidiLink::discard_inputs() noexcept {
  midi::RtItem it;
  for (std::uint32_t p = 0; p < midi::kPortCount; ++p) {
    for (std::uint32_t i = 0; i < midi::kRtInRingSize && in_ring_[p].pop(it); ++i) {
      ++stats_.rings_discarded;
    }
  }
}

void MidiLink::drain_inputs() noexcept {
  midi::TransportEvent ev[kEventCap];
  midi::RtItem pass[kPassCap];
  midi::MtcEvent mtc[kMtcCap];
  std::uint32_t nev = 0;
  if (cc_ != nullptr) {  // OS-MIDIREC-P1 S3 (A8): the stamp of this block's MIDI, only while a take is open (the main loop's release store of the track)
    const std::uint8_t tt = cc_->take_track();
    std::uint8_t pat = 0;
    std::uint32_t tick = 0;
    if (tt != midi::CcInput::kNoTrack && seq_.capture_stamp(tt, pat, tick)) {
      cc_->set_stamp(pat, tick);
    } else {
      cc_->clear_stamp();
    }
  }
  for (std::uint32_t p = 0; p < midi::kPortCount; ++p) {
    // Both rings are drained in every mode, so that neither fills: with the clock not followed the timing bytes are counted and dropped by MidiIn.
    const bool chasing = cfg_.mtc_in != 0u;  // OS-MTC B3: the sink only when MTC IN is on (MidiIn takes the time code of its own port only); without it the call is what it was
    const midi::MidiIn::Drained d = in_.drain(in_ring_[p], map_, dll_, kDrainItems, ev + nev, kEventCap - nev, pass, kPassCap, chasing ? mtc : nullptr, chasing ? kMtcCap : 0u);
    nev += d.events;
    for (std::uint32_t i = 0; i < d.mtc; ++i) {
      chase_notice(chaser_.feed(mtc[i], map_));
    }
    chase_frames_ += d.mtc;  // reset on a relocation notice (and on chase_reset), not on resyncs: the fit needs this many fresh frames after a move
    if (cc_ != nullptr) {
      for (std::uint32_t i = 0; i < d.passed; ++i) {
        cc_->feed(pass[i]);  // control changes for MIDI Learn (bounded: kPassCap bytes a call)
      }
    } else {
      stats_.passed_dropped += d.passed;
    }
  }
  for (std::uint32_t i = 0; i < nev; ++i) {
    const midi::TransportEvent& e = ev[i];
    if (!cfg_.transport_in) {
      ++stats_.transport_ignored;  // E-CFG-03: TRANSPORT IN Off: the clock is followed (the DLL), the transport bytes are not acted on
      continue;
    }
    const bool begins = e.kind == midi::TransportEvent::Kind::Start || e.kind == midi::TransportEvent::Kind::Continue;
    if (begins && !seq_.following() && dll_.has_phase() && !want_release_) {
      // The master's own start restarts the run on the master's clock anyway, so the loop may be attached even though the tracker is playing on its own.
      seq_.set_follow(&dll_, in_offset_us());
      ++stats_.follow_attached;
    }
    seq_.apply(e);
    ++stats_.events_applied;
    st_transport_.store(st_transport_.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
  }
}

// ---- following ----------------------------------------------------------------------------------------------------------------

// Lets go of the master: the sequencer is stopped first (its timeline is the loop's: it cannot be handed back to the internal one while it runs), and once it has stopped
// (the next block) the loop is detached and started over. Called every block while `want_release_` or while the follow has to end.
void MidiLink::release_follow() noexcept {
  if (!seq_.following()) {
    want_release_ = false;
    stop_requested_ = false;
    return;
  }
  if (seq_.playing()) {
    if (!stop_requested_) {
      seq_.stop();
      stop_requested_ = true;
    }
    return;
  }
  seq_.clear_follow();
  stop_requested_ = false;
  want_release_ = false;
  reset_loop(last_block_end_);
}

void MidiLink::manage_follow(std::uint64_t block_start) noexcept {
  const bool slave = slave_of(cfg_);
  if (!slave || want_release_) {
    release_follow();
    return;
  }
  const core::SQ32 now_rel = (static_cast<core::SQ32>(block_start) - static_cast<core::SQ32>(dll_.origin())) * static_cast<core::SQ32>(core::kQ32One);
  const bool alive = dll_.has_phase() && !dll_.stale(now_rel, kStaleClocks);
  if (seq_.following()) {
    if (!alive) {
      ++stats_.stale_releases;
      want_release_ = true;  // the master went quiet (or the loop was reset under us): stop, then detach
      release_follow();
    }
  } else if (alive && !seq_.playing()) {
    seq_.set_follow(&dll_, in_offset_us());
    ++stats_.follow_attached;
  } else if (dll_.has_phase() && !alive) {
    reset_loop(block_start);  // a stale loop with nobody attached: forget it so that a master that comes back is acquired afresh
  }
}

void MidiLink::publish_status(std::uint64_t block_start) noexcept {
  const bool has = dll_.has_phase();
  const core::SQ32 now_rel = (static_cast<core::SQ32>(block_start) - static_cast<core::SQ32>(dll_.origin())) * static_cast<core::SQ32>(core::kQ32One);
  const bool present = slave_of(cfg_) && has && !dll_.stale(now_rel, kStaleClocks);
  const std::uint32_t flags = (seq_.following() ? 1u : 0u) | (present ? 2u : 0u) | (present && dll_.locked() ? 4u : 0u);
  st_flags_.store(flags, std::memory_order_relaxed);
  st_bpm_.store(present ? dll_.bpm_x100() : 0u, std::memory_order_relaxed);
  st_clocks_.store(in_.stats().clocks, std::memory_order_relaxed);
  float e = dll_.last_error_samples();
  e = e < 0.0f ? -e : e;
  const float us = e * (static_cast<float>(core::kRefCyclesPerSample.num) / static_cast<float>(core::kRefCyclesPerSample.den)) / 180.0f;  // cycles per sample / 180 cycles per us
  st_error_us_.store(present && us < 1.0e6f ? static_cast<std::uint32_t>(us) : 0u, std::memory_order_relaxed);

  // OS-MTC B3: what the Config row says about the chase.
  std::uint32_t m = 0;
  std::uint32_t time = 0;
  if (cfg_.mtc_in != 0u) {
    using St = midi::mtc::Chaser::State;
    midi::MtcState ms = midi::MtcState::Waiting;
    if (chase_ph_ == ChasePhase::PastEnd) {
      ms = midi::MtcState::PastEnd;
    } else if (chaser_.refused()) {
      ms = midi::MtcState::Rate;
    } else {
      switch (chaser_.state()) {
        case St::Idle: ms = midi::MtcState::Waiting; break;
        case St::Reading:
        case St::Acquired: ms = midi::MtcState::Reading; break;
        case St::Armed: ms = midi::MtcState::Armed; break;
        case St::Locked: ms = midi::MtcState::Locked; break;
        case St::Freewheel: ms = midi::MtcState::Freewheel; break;
      }
    }
    m = static_cast<std::uint32_t>(ms) | (static_cast<std::uint32_t>(chaser_.code()) << 8);
    const midi::mtc::Time t = chaser_.last_time();
    time = (static_cast<std::uint32_t>(t.h) << 24) | (static_cast<std::uint32_t>(t.m) << 16) | (static_cast<std::uint32_t>(t.s) << 8) | t.f;
  }
  st_mtc_.store(m, std::memory_order_relaxed);
  st_mtc_time_.store(time, std::memory_order_relaxed);
  st_mtc_ppm_.store(cfg_.mtc_in != 0u ? chaser_.rate_ppm() : 0, std::memory_order_relaxed);
  st_mtc_rel_.store(stats_.chase_relocations, std::memory_order_relaxed);
}

// ---- OS-MTC B3: the chase ---------------------------------------------------------------------------------------------------

// MTC START as a time in Q32 samples (it offsets the incoming time: the song's first step is this far into the master's day).
core::Q32 MidiLink::mtc_start_q32() const noexcept {
  // The song's start is the master's label hh:00:00:00 at the chaser's rate code. At 29.97 drop-frame that label is 3.6 ms short of the hour per hour
  // (frames_of_label counts the labels the drop skips), which qf_time_real turns into real samples; the plain quarter-frame count would start the song late.
  const std::uint8_t code = chaser_.code();
  return midi::mtc::qf_time_real(4u * midi::mtc::frames_of_label(midi::mtc::Time{static_cast<std::uint8_t>(cfg_.mtc_hour), 0, 0, 0}, code), code);
}

// The master's rate as the song's speed, Q16 (65536 = 1.0).
std::uint32_t MidiLink::chase_rate_q16() const noexcept {
  const std::int64_t r = 65536 + static_cast<std::int64_t>(chaser_.rate_ppm()) * 65536 / 1000000;
  return r < 0 ? 0u : static_cast<std::uint32_t>(r);
}

// Everything the chase holds starts over: the chaser, the phase, a run that was the master's (stopped), the request in flight (its stamp is never answered: the stamp moves on).
void MidiLink::chase_reset() noexcept {
  chaser_.reset();
  if (seq_.chase_run()) {
    seq_.chase_stop();
  }
  seq_.set_rate_trim(Sequencer::kTrimOne);
  seq_.chase_take();
  chase_ph_ = ChasePhase::Wait;
  chase_loc_ = false;
  chase_good_ = false;
  chase_margin_ = static_cast<std::uint32_t>(core::samples_in_ms(50));
  chase_seen_ = seq_.chase_steps();
  chase_relocated_ = false;
  chase_first_ask_ = 0;
  chase_reloc_at_ = 0;
  chase_frames_ = 0;
}

void MidiLink::chase_notice(midi::mtc::Chaser::Notice n) noexcept {
  using N = midi::mtc::Chaser::Notice;
  switch (n) {
    case N::Relocated:
    case N::Stopped:
      // The master moved or stopped (or left the rates we follow): the run that was its stops, and the question starts over from the new time.
      ++stats_.chase_relocations;
      chase_relocated_ = true;
      chase_first_ask_ = 0;  // the next question starts a new settle window
      chase_reloc_at_ = 0;   // ... and this relocation re-anchors the settle max (manage_chase stamps it on the next block)
      chase_frames_ = 0;     // the fit re-converges from here
      if (seq_.chase_run()) {
        seq_.chase_stop();
      }
      seq_.set_rate_trim(Sequencer::kTrimOne);
      chase_ph_ = ChasePhase::Wait;
      chase_good_ = false;
      chase_margin_ = static_cast<std::uint32_t>(core::samples_in_ms(50));
      break;
    case N::Locate:
      if (!seq_.playing() && !seq_.chase_run()) {
        chase_ask(last_block_end_, true);
      }
      break;
    case N::Acquired:
    case N::None:
      break;
  }
}

// The question for the main loop: the master's time `chase_margin_` ahead (or the full frame's time, for a locate), minus MTC START (a time before it is song time 0: step 1 waits armed).
void MidiLink::chase_ask(std::uint64_t block_start, bool locate) noexcept {
  core::Q32 tm = 0;
  if (locate) {
    tm = chaser_.locate().time;
  } else if (!chaser_.master_time_at(block_start + chase_margin_, tm)) {
    return;  // no phase yet: asked again next block
  }
  const core::Q32 off = mtc_start_q32();
  const std::uint64_t song = tm > off ? core::q32_floor(tm - off) : 0u;
  if (!locate && chase_first_ask_ == 0u) {
    chase_first_ask_ = block_start;  // stamped once per start/reset, only by real questions: re-asks and locate asks do not move it
  }
  chase_stamp_ = chase_stamp_ >= 255u ? std::uint8_t{1} : static_cast<std::uint8_t>(chase_stamp_ + 1u);
  chase_mode_ = seq_.mode();
  chase_pattern_ = static_cast<std::uint8_t>(seq_.pattern_id_ & 0xFFu);
  chase_epoch_ = chaser_.epoch();
  chase_loc_ = locate;
  chase_asked_at_ = block_start;
  chase_ph_ = ChasePhase::Asked;
  chase_req_time_.store(static_cast<std::uint32_t>(song > 0xFFFFFFFFu ? 0xFFFFFFFFu : song), std::memory_order_relaxed);
  chase_req_.store((static_cast<std::uint32_t>(chase_stamp_) << 24) | (locate ? 1u << 17 : 0u) | (chase_mode_ == PlayMode::Song ? 1u << 16 : 0u) | (static_cast<std::uint32_t>(chase_pattern_) << 8), std::memory_order_release);
  ++stats_.chase_requests;
}

// The main loop's answer is there: start the run at the sample the master reaches the step, or ask again.
void MidiLink::chase_answered(std::uint64_t block_start) noexcept {
  const Sequencer::ChaseAnswer a = seq_.chase_answer();
  seq_.chase_take();
  if (chase_loc_) {
    chase_ph_ = ChasePhase::Wait;
    chase_loc_ = false;
    if (a.pos != Sequencer::kChasePastEnd) {
      seq_.chase_locate(chase_mode_ == PlayMode::Song ? static_cast<std::uint32_t>(a.pos >> 7) : 0xFFFFu, a.pos & 0x7Fu);
      ++stats_.chase_locates;
    }
    return;
  }
  if (a.pos == Sequencer::kChasePastEnd) {
    chase_ph_ = ChasePhase::PastEnd;  // silence; a relocation of the master asks again
    return;
  }
  std::uint64_t at = 0;
  const bool have = chaser_.sample_at_master_time((static_cast<core::Q32>(a.song_samples) << core::kQ32Frac) + mtc_start_q32(), at);
  if (!have || at < block_start + kChaseMinLead) {
    // Too late (the main loop was slow, or the margin was thin): ask again further ahead.
    ++stats_.chase_retries;
    chase_margin_ = chase_margin_ >= core::samples_in_ms(1600) ? chase_margin_ : chase_margin_ * 2u;
    chase_ph_ = ChasePhase::Wait;
    return;
  }
  if (!chaser_.arm(chase_epoch_)) {
    chaser_.disarm();
    ++stats_.chase_retries;  // the mapping changed since the question: the answer is for another time
    chase_ph_ = ChasePhase::Wait;
    return;
  }
  // The mapping may still be settling (right after a relocation): hold the start until the sample the mapping gives the step stops moving, then begin there.
  chase_pend_target_ = (static_cast<core::Q32>(a.song_samples) << core::kQ32Frac) + mtc_start_q32();
  chase_pend_at_ = at;
  chase_pend_since_ = block_start;
  chase_ph_ = ChasePhase::Settling;
}

// The held start: begin when the sample the current mapping gives the step has held still over the window (a fit that is still settling would put the first steps off their map
// time), when it went too late, or after the max wait.
void MidiLink::chase_settling(std::uint64_t block_start) noexcept {
  std::uint64_t at = 0;
  if (!chaser_.sample_at_master_time(chase_pend_target_, at)) {
    chaser_.disarm();
    ++stats_.chase_retries;
    chase_ph_ = ChasePhase::Wait;
    return;
  }
  if (at < block_start + kChaseMinLead) {
    // Too late (the settle wait ate the margin): ask again further ahead — the margin doubles, so that at high tempo (a step shorter than the settle
    // window) the start sample stays ahead of the block long enough for the window to elapse.
    chaser_.disarm();
    ++stats_.chase_retries;
    chase_margin_ = chase_margin_ >= core::samples_in_ms(1600) ? chase_margin_ : chase_margin_ * 2u;
    chase_ph_ = ChasePhase::Wait;
    return;
  }
  if ((!chase_relocated_ || chase_frames_ >= kChaseSettleFrames) && block_start - chase_pend_since_ >= kChaseSettleWindow) {
    if (at > chase_pend_at_ ? at - chase_pend_at_ <= kChaseSettleTol : chase_pend_at_ - at <= kChaseSettleTol) {
      seq_.chase_go(at, chase_mode_, chase_pattern_, chase_rate_q16());
      ++stats_.chase_starts;
      chase_ph_ = ChasePhase::Running;
      chase_good_ = false;
      chase_seen_ = seq_.chase_steps();
      chase_relocated_ = false;
      chase_first_ask_ = 0;
      chase_reloc_at_ = 0;
      return;
    }
    chase_pend_at_ = at;  // still moving: watch on from here
    chase_pend_since_ = block_start;
  }
  const std::uint64_t anchor = chase_reloc_at_ != 0u ? chase_reloc_at_ : chase_first_ask_;  // the max hangs off the relocation itself, not off re-asks
  if (anchor != 0u && block_start - anchor > kChaseSettleMax) {  // a start that never settles begins anyway with the latest mapping
    seq_.chase_go(at, chase_mode_, chase_pattern_, chase_rate_q16());
    ++stats_.chase_starts;
    chase_ph_ = ChasePhase::Running;
    chase_good_ = false;
    chase_seen_ = seq_.chase_steps();
    chase_relocated_ = false;
    chase_first_ask_ = 0;
    chase_reloc_at_ = 0;
  }
}

// Once a step: the run reaches the song time chase_map() at the sample chase_end(); the master is at chaser_.master_time_at(that) then. The difference sets the rate.
void MidiLink::chase_measure() noexcept {
  const std::uint32_t steps = seq_.chase_steps();
  if (steps == chase_seen_) {
    return;
  }
  chase_seen_ = steps;
  const std::uint64_t at = seq_.chase_end();
  core::Q32 tm = 0;
  if (!chaser_.master_time_at(at, tm)) {
    return;
  }
  // The master's day wraps at 24:00:00:00 while the run's song time keeps counting: the phase error is the signed difference of the two
  // modulo the day (a day in Q32.32 does not fit a signed word, so this is done in whole samples, where it does).
  const std::uint64_t day = core::q32_floor(midi::mtc::qf_time_real(midi::mtc::quarter_frames_per_day_of_code(chaser_.code()), chaser_.code()));
  // chase_map() + mtc_start_q32() is a Q32 sum: it wraps at 2^32 samples (27.3 h), not at the master's day. That is out of reach: the song is at most
  // kMaxSongPositions x kMaxPatternLength steps at the slowest tempo (255 x 128 x 16,537 samples = 3.5 h) and MTC START is at most 23 h, so the sum stays under 27 h.
  const std::uint64_t a = core::q32_floor(seq_.chase_map() + mtc_start_q32());
  const std::uint64_t b = core::q32_floor(tm);
  std::int64_t e_samples = static_cast<std::int64_t>(a - b) % static_cast<std::int64_t>(day);
  if (e_samples > static_cast<std::int64_t>(day) / 2) {
    e_samples -= static_cast<std::int64_t>(day);
  } else if (e_samples < -static_cast<std::int64_t>(day) / 2) {
    e_samples += static_cast<std::int64_t>(day);
  }
  const std::int64_t hz = static_cast<std::int64_t>(core::samples_in_ms(1000));
  const std::int64_t e_us = e_samples * 1000000 / hz;
  st_mtc_err_us_.store(static_cast<std::int32_t>(e_us), std::memory_order_relaxed);
  // r = r_m x (1 - e / tau), tau one second: a phase error of e samples moves the speed by e / hz.
  std::int64_t target = static_cast<std::int64_t>(chase_rate_q16()) - e_samples * 65536 / hz;
  const std::int64_t cur = static_cast<std::int64_t>(seq_.rate_trim());
  target = target > cur + kChaseSlewQ16 ? cur + kChaseSlewQ16 : (target < cur - kChaseSlewQ16 ? cur - kChaseSlewQ16 : target);
  seq_.set_rate_trim(target < 0 ? 0u : static_cast<std::uint32_t>(target));
  const std::int64_t abs_us = e_us < 0 ? -e_us : e_us;
  if (abs_us < kChaseGoodUs) {
    if (!chase_good_) {
      chase_good_ = true;
      chase_good_since_ = at;
    }
    // 8 quarter frames: two frames of the master's rate
    if (at - chase_good_since_ >= core::samples_in_ms(2000) / chaser_.fps()) {
      (void)chaser_.lock();
    }
  } else {
    chase_good_ = false;
  }
}

void MidiLink::manage_chase(std::uint64_t block_start) noexcept {
  if (cfg_.mtc_in == 0u) {
    return;
  }
  if (chase_relocated_ && chase_reloc_at_ == 0u) {
    chase_reloc_at_ = block_start;  // the relocation itself anchors the settle max
  }
  chase_notice(chaser_.poll(block_start));
  switch (chase_ph_) {
    case ChasePhase::Running:
      if (!seq_.chase_run()) {
        // The run ended without us: the song's end (Sequencer: the chase does not loop). Silence, the chaser stays on the master; a relocation asks again.
        chaser_.disarm();
        chase_ph_ = ChasePhase::PastEnd;
      } else if (seq_.playing()) {
        chase_measure();
      }
      break;
    case ChasePhase::Asked:
      if (seq_.chase_answer().stamp == chase_stamp_) {
        chase_answered(block_start);
      } else if (block_start - chase_asked_at_ > core::samples_in_ms(500)) {
        ++stats_.chase_retries;  // the main loop did not answer (busy with the card): ask again, as far ahead as before
        chase_ph_ = ChasePhase::Wait;
      }
      if (seq_.chase_answer().stamp != 0u && seq_.chase_answer().stamp != chase_stamp_) {
        seq_.chase_take();  // an answer to a question we no longer wait for
      }
      break;
    case ChasePhase::PastEnd:
      break;
    case ChasePhase::Wait:
      // A run of the unit's own clock (the user's Play while the master is silent) is left alone: the master takes over once it is stopped.
      // After a relocation, wait for the fit to re-converge (kChaseSettleFrames fresh quarter frames) before asking: a question on a
      // mid-transition mapping can be answered for a wrong time (past the song's end, say) and would park the chase in PastEnd.
      if (!seq_.playing() && !seq_.chase_run() && chaser_.state() == midi::mtc::Chaser::State::Acquired &&
          (!chase_relocated_ || chase_frames_ >= kChaseSettleFrames)) {
        chase_ask(block_start, false);
      }
      break;
    case ChasePhase::Settling:
      chase_settling(block_start);
      break;
  }
}

// ---- the settings the device file carries ---------------------------------------------------------------------------------------

MidiConfig sync_config_from_settings(const model::Settings& device, const MidiConfig& base) noexcept {
  namespace ds = model::device_setting;
  const auto choice = [&device](unsigned field, unsigned top) noexcept {
    const float v = device.get(field);
    return v >= static_cast<float>(top) ? top : (v >= 1.0f ? static_cast<unsigned>(v) : 0u);
  };
  MidiConfig c = base;
  // MIDI OUT: DIN, USB, DIN + USB. Clock and transport and the notes of MIDI tracks share it.
  static constexpr std::uint8_t kPortSets[3] = {midi::kPortDin, midi::kPortUsb, static_cast<std::uint8_t>(midi::kPortDin | midi::kPortUsb)};
  const std::uint8_t out = kPortSets[choice(ds::MidiOutPort, 2)];
  c.out_ports = out;
  c.note_ports = out;
  const unsigned out_channel = choice(ds::MidiOutChannel, 16);  // 1..16; 0 only for a value below the range
  c.notes_channel = static_cast<std::uint8_t>(out_channel > 0u ? out_channel - 1u : 0u);
  const unsigned clock_in = choice(ds::ClockIn, 2);
  if (clock_in != 0u) {
    c.mode = SyncMode::Slave;
    c.in_port = clock_in == 1u ? midi::Port::Usb : midi::Port::Din;
  } else {
    const unsigned mode = choice(ds::ClockMode, 2);
    c.mode = mode == 2u ? SyncMode::Slave : (mode == 1u ? SyncMode::Master : SyncMode::Off);
    if (mode == 2u) {
      c.in_port = choice(ds::MidiInPort, 2) == 1u ? midi::Port::Usb : midi::Port::Din;  // USB only when it is the only port listened to
    }
  }
  c.transport_in = device.get(ds::TransportIn) >= 0.5f;  // E-CFG-03: Off (0) / On (1, the default)
  c.transport_out = device.get(ds::TransportOut) >= 0.5f;
  c.mtc_fps = midi::mtc::fps_of_setting(choice(ds::MtcOut, midi::mtc::kSettingMax));  // OS-MTC A3: Off, 24, 25, 30 fps
  c.mtc_hour = static_cast<std::uint8_t>(choice(ds::MtcStart, midi::mtc::kMaxHour));    // 00..23
  c.mtc_in = static_cast<std::uint8_t>(choice(ds::MtcIn, 2));                            // OS-MTC B3: Off, DIN, USB
  const float corr = device.get(ds::ClockSync);
  c.sync_correction = static_cast<std::int8_t>(corr < -6.0f ? -6 : (corr > 6.0f ? 6 : static_cast<int>(corr)));
  return midi::sanitized(c);
}

static_assert(model::kMidiPorts == midi::kPortCount && model::kMidiPortMask == midi::kPortMask && model::kMidiCcUnassigned == midi::kCcUnassigned &&
                  model::kMaxMidiOffsetDms == midi::kMaxOffsetDms && model::kMidiCcSlots == sizeof(midi::MidiConfig::cc_number) &&
                  model::kMidiFollowMax == static_cast<std::uint8_t>(midi::FollowTightness::Loose),
              "model/project_state.h mirrors midi/config.h");

model::MidiState project_midi_state(const MidiConfig& cfg) noexcept {
  const MidiConfig c = midi::sanitized(cfg);
  model::MidiState s;
  s.out_ports = c.out_ports;
  s.note_ports = c.note_ports;
  s.notes_channel = c.notes_channel;
  s.follow = static_cast<std::uint8_t>(c.follow);
  for (std::uint32_t i = 0; i < model::kMidiCcSlots; ++i) {
    s.cc_number[i] = c.cc_number[i];
  }
  for (std::uint32_t p = 0; p < model::kMidiPorts; ++p) {
    s.in_offset_dms[p] = c.in_offset_dms[p];
    s.out_offset_dms[p] = c.out_offset_dms[p];
  }
  return s;
}

MidiConfig apply_project_midi(const MidiConfig& base, const model::MidiState& s) noexcept {
  MidiConfig c = base;
  c.out_ports = s.out_ports;
  c.note_ports = s.note_ports;
  c.notes_channel = s.notes_channel;
  c.follow = static_cast<midi::FollowTightness>(s.follow);
  for (std::uint32_t i = 0; i < model::kMidiCcSlots; ++i) {
    c.cc_number[i] = s.cc_number[i];
  }
  for (std::uint32_t p = 0; p < model::kMidiPorts; ++p) {
    c.in_offset_dms[p] = s.in_offset_dms[p];
    c.out_offset_dms[p] = s.out_offset_dms[p];
  }
  return midi::sanitized(c);
}

}  // namespace o3os::seq
