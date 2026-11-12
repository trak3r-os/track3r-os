// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// MIDI sync end to end, the render side (OS-005c2; spec OS-005 "005c2 - MIDI drivers on the MK66" and "005c"; ADR 0013 D1 and D5; radar card midi-clock-sync and its
// orchestrator decisions). One object that sits between the sequencer, the MIDI timing core of OS-005c (midi/) and the device drivers of the platform:
//
//   device interrupts (platform)                    render (this class, once a block)                   device interrupts (platform)
//   UART4 RX  --stamp--> din_in()  --+                                                                +--> din() --PIT compare--> UART4 TX
//   USB0      --stamp--> usb_in()  --+--> MidiIn::drain --> ClockDll --> Sequencer (follow, transport) |
//                                                  Sequencer::step --> MidiOut::send/flush -----------+--> usb_out() --SOF--> USB bulk IN
//
// WHAT A BLOCK DOES (MidiLink::step, the engine's EventSource::step; audio/event_source.h):
//   1. publish the time base: the timeline sample the block starts at and the cycle-counter value at which it is HEARD (the platform's hook), to TimeRef (for the
//      interrupts) and to the ClockMap (for the stamps of incoming bytes);
//   2. take a new MidiConfig if the main loop wrote one (seqlock, never waits) and act on it;
//   3. drain both ports' midiRt rings: clocks feed the follow loop when this tracker is a Slave (the followed port's only), Start / Continue / Stop / SPP become
//      TransportEvents for the sequencer, every other byte goes to the control-change parser of MIDI Learn (attach_cc; midi/cc_in.h) or, with none attached, is counted and
//      dropped (the notes-in path is another slice's: OS-013a TODO);
//   4. look after the follow state: attach the sequencer to the loop once the master's clock is there, let go (stopping first) when the master has gone quiet or the
//      mode changed;
//   5. Sequencer::step, then every MIDI event of the block goes through MidiOut::send, a Stopped event also asks MidiOut::stop_all (the notes still owed get their
//      offs), and MidiOut::flush hands the ports what is due inside the horizon (2.9 ms);
//   6. if anything went to the DIN ring, pend the emitter's interrupt (the platform's kick hook): it decides from the queues, so a pend is always safe.
//
// MODES (MidiConfig::mode; midi/config.h).
//   Off     no clock out, no clock in, no transport bytes either way. Notes of MIDI tracks still go to `note_ports`.
//   Master  Sequencer::Config::emit_clock is on while `out_ports` is not empty: while the tracker plays, a 24 ppqn clock (placed to a fraction of a sample: the emitter's
//           real-time lane, "clock is sacred"), Start (from the first step) or SPP + Continue (from anywhere else) and Stop go out on `out_ports`. With clock out on,
//           a Play begins one block later (Sequencer::Config::start_lead), so that SPP and the Start byte are on the wire before the first note. Takes effect at the
//           next Play.
//   Slave   the clock and the transport come from `in_port`: its clock goes through the ClockDll (second-order loop, slews, never jumps), and the sequencer's timeline
//           IS the loop's (Sequencer::set_follow) with the port's IN OFFSET; Start begins the run on the clock MidiIn names, Continue resumes (after an SPP, from there),
//           Stop stops, SPP locates. `out_ports` relays clock and transport, on the followed time.
//   TRANSPORT IN / OUT (MidiConfig::transport_in / transport_out; E-CFG-03, new Community OS behaviour; with Off as the mode they do nothing). IN Off: drain_inputs drops the Start, Continue,
//           Stop and SPP of the followed port (counted in Stats::transport_ignored); the clock is still followed, so the Play key starts on the master's next clock, and a Stop of the master
//           does not stop the tracker (only a master gone quiet does: manage_follow). OUT Off: Sequencer::Config::emit_transport is false, the clock goes out (Master) or is relayed (Slave)
//           without Start, Continue, Stop or SPP, and start_lead stays 0 (nothing to put on the wire first). Both take effect at the next Play; a run keeps what it began with.
// MTC IN (OS-MTC B3; docs/specs/OS-MTC.md 4.2 to 4.6; new Community OS behaviour). With MidiConfig::mtc_in not Off the link chases the time code of that port: the quarter frames and full frames MidiIn::drain hands over feed
// a midi::mtc::Chaser; when it has the master's time (Acquired) the render asks the main loop which step of the song starts at or after that time plus a margin (chase_request(): app::ChaseSync answers with
// song_position_at over the sequencer's command ring), computes the local sample at which the master reaches it and starts the run there (Sequencer::chase_go: the tempo and swing the song has at that step,
// the swing pair's parity, the master's rate as the song's speed). From then on, once a step, the run's phase against the master (Sequencer::chase_end / chase_map against the chaser's master_time_at) sets the
// rate trim (r = r_m x (1 - e / tau), tau 1 s, limited to +-6 % and to 3 % a step): a phase error is pulled in over about a second and never jumps, swing and the clock out keep their ratios. Locked when |e| is below 1 ms
// for 8 quarter frames. A relocation or a stop of the master stops the run (the chaser tells: Relocated, Stopped) and the request starts over; a time past the song's end is silence (the unit waits for the master to
// locate back: PastEnd, Q9 a); a time before it arms step 1 for the sample at which the master reaches MTC START; a full frame while the unit is stopped moves its cursor (Locate). While the chase runs the
// main loop's Play, Stop, Resume, Locate and slot jumps do nothing (Q8 a, Sequencer::chase_run); a Play of the user while the master is silent (or before it started) is a run of the unit's own clock, and the
// master takes over only once that run is stopped. MTC IN not Off also turns the clock follow off (CLOCK IN and CLOCK MODE Slave are ignored: a Slave acts as a Master for the clock out) and mutes MTC OUT.
//
// NEW COMMUNITY OS BEHAVIOUR (decided here, each a constant or one branch, all reversible): (a) a master that has gone quiet for kStaleClocks (48
// clocks, two beats) while the tracker follows it stops the transport and releases the loop (a slave that kept flywheeling at the last tempo would play on
// for ever after a cable was pulled); (b) the Play key under an external clock starts on the master's next clock, quantised to its grid (Sequencer::next_follow_clock),
// where the release-notes item PAR-122 (count-in, wait for the master) is not covered, and what the official firmware does is an open research question; (c) a change of mode or of the followed port while following stops the
// transport first; (d) the loop is attached to the sequencer only while the tracker is stopped or when the master's own Start / Continue arrives (that one restarts the
// run on the master's clock anyway): a tracker that is playing on its own clock when the master first appears keeps playing on its own until it is stopped.
//
// LIVE MIDI NOTES (OS-INSTUI S2b; NEW COMMUNITY OS BEHAVIOUR). A pad on a MIDI instrument sends its note: the pad's note-on/off travel the engine's live queue (audio/live_notes.h,
// one queue, one consumer, no new ring), Engine::play_live offers every event it pops to live_event() (EventSource::live) before it acts, and a PAD note-on whose instrument is of
// kind Midi is taken here (true: the engine starts no voice) and becomes a MidiNoteOn on the instrument's channel (the sequencer's rule: channel 1..16 -> 0..15, velocity 127 for the pad's
// default, 1..127 as given) through MidiOut::send, so DIN/USB timing, running status, the hang book and the panic are the router's own. A pad's Off (and an AllOff of the pads, or of
// all sources) ends the note with the channel and note stored at the note-on (`live_pad_`, indexed by pad 0..47, render only): the release never looks at the instrument, so a change
// of instrument, a project load or a lost key can not leave a note on. Two things order a live note against the sequencer's: (1) play_live runs after this class's step() of the
// block, whose flush has gone, so the note is stamped at the first sample of the NEXT block (last_block_end_, offset 0): it is staged behind every sequencer event of the block it
// was popped in and ahead of every one of the next block's (a stable sort: equal due times keep the order they were staged in), so each port's ring stays in due order; (2) it
// is one block (1.45 ms) later than the voice path, which starts at sample 0 of the block it is popped in. A live note is not muted by the track strip (a pad has no track on the MIDI side) and
// Stopped (stop_all) takes it with the rest: the pad's later Off is then not owed and is dropped (MidiOut::stats().stray_offs). Per block it is bounded by the live queue: kLivePerBlock
// events, each one MidiOut::send (an AllOff at most kLivePads).
// OS-PIANOBEND (docs/specs/OS-PIANOBEND.md): a LiveKind::Bend of a MIDI instrument (semitones -2..+2, the PIANO strip) is a MidiPitchBend of pitch_bend_value(semitones x 100, midi_pb_range) on the instrument's
// route and is taken here (true: the voices get no bend); 0, or an instrument that is not MIDI, sends the centre to the route last bent (`bend_route_`) and returns false so the voices are put back too; an AllOff of the
// pads or of all sources centres it as well. A stop does not centre: the strip's release does.
// Known limits (review, 2026-10-14): the hang book is per channel and note and shared with the sequencer, so a sequencer note-off on the same channel and note cuts a held pad
// note and a pad release cuts a sounding sequencer note (the receiver has one voice per note); a pad pressed in the few blocks after a Stop can be taken by
// the panic pump; a pad's off that MidiOut's staging refuses (a driver fault) is not sent again, so that note hangs on the receiver until the next Stop or panic, as a sequencer off would.
//
// CONTEXTS (the ISR contracts; each shared object has ONE writer and ONE reader, and says which):
//   render (priority 128)     the only caller of step(), of everything the sequencer and MidiOut have, of MidiIn and the ClockDll.
//   main loop                 set_config() (SeqLockBox: one writer, the render reads without waiting), status() (relaxed single-word loads).
//   UART4 RX interrupt (48)   the ONE producer of din_in() (midi/din_io.h DinRx).
//   USB0 interrupt (64)       the ONE producer of usb_in(), the ONE consumer of usb_out() (midi/usb_bind.h), a reader of time_ref().
//   PIT0 interrupt (96)       the ONE consumer of din() (midi/din_io.h DinTx), a reader of time_ref(). All three interrupts are above the render's priority, which TimeRef's
//                             protocol needs. The render is the ONE producer of din() and usb_out() and the ONE writer of time_ref().
//
// REAL TIME. Everything here is bounded: at most kDrainItems items from each ring and kEventCap transport events per block, MidiOut's own bounds, no allocation, no
// blocking, no printf, no double. It runs from flash, once a block, inside the render's budget (the cost is an HW-B row: `MidiIn::drain` + `ClockDll::on_clock`).
//
// SIZE. About 23 KB of SRAM when instantiated (the emitter's rings 5.3 KB, USB out 5.2 KB, the router 6.5 KB, the two midiRt rings 4.2 KB, the loop 0.4 KB): in the
// platform it is a static in SRAM_U (platform/mk66/src/app_main.cpp), against ADR 0013 D7's "rings" row.

#include <o3os/audio/event_source.h>
#include <o3os/audio/events.h>
#include <o3os/audio/live_notes.h>
#include <o3os/core/qtime.h>
#include <o3os/midi/cc_in.h>
#include <o3os/midi/clock_dll.h>
#include <o3os/midi/config.h>
#include <o3os/midi/din.h>
#include <o3os/midi/in.h>
#include <o3os/midi/mtc_in.h>
#include <o3os/midi/out.h>
#include <o3os/midi/time_ref.h>
#include <o3os/midi/usb_out.h>
#include <o3os/model/settings.h>
#include <o3os/seq/sequencer.h>

#include <atomic>
#include <cstdint>

namespace o3os::seq {

// What the platform gives the link.
struct MidiLinkHooks {
  // The cycle-counter value at which the first sample of the block being rendered is HEARD (the boundary stamp of its render plus the render-ahead of the TX ring).
  // Called once per block, in the render context.
  std::uint32_t (*heard_at)(void* ctx) noexcept = nullptr;
  // Pends the DIN emitter's interrupt. Called in the render context after items went to the DIN ring. May be null (a platform without a DIN port, a test).
  void (*kick)(void* ctx) noexcept = nullptr;
  void* ctx = nullptr;
};

class MidiLink {
 public:
  static constexpr std::uint32_t kDrainItems = 128;    // items taken from each ring per block (the rest waits)
  static constexpr std::uint32_t kEventCap = 8;        // transport events per block (a Start, an SPP and a Stop are three)
  static constexpr std::uint32_t kPassCap = 16;        // bytes that are not timing, per drain call: counted and dropped
  static constexpr std::uint32_t kMtcCap = 16;         // OS-MTC B3: time code messages per drain call (a quarter frame is 10 ms: 16 are far more than a block brings)
  static constexpr std::uint32_t kStaleClocks = 48;    // a master quieter than this many clock periods is gone
  static constexpr std::uint32_t kStartLead = 64;      // samples: Sequencer::Config::start_lead with clock out on (one block)

  MidiLink(Sequencer& sequencer, const MidiLinkHooks& hooks) noexcept;

  // ---- the main loop ------------------------------------------------------------------------------------------------------------
  // Hands the engine the user's MIDI settings (clamped into range, midi::sanitized). The latest call wins; the render acts on it at its next block. The main loop is the
  // ONE writer.
  void set_config(const midi::MidiConfig& c) noexcept;
  // What set_config() was last given, as the main loop sees it (the defaults before the first call).
  const midi::MidiConfig& config() const noexcept { return box_.last_written(); }
  midi::SyncStatus status() const noexcept;
  // OS-MTC B3, the chase's question to the main loop (app::ChaseSync): the master's time, minus MTC START, in whole samples, and a word: stamp (bits 24..31, 0 = none yet) | locate only (bit 17: a full frame while
  // stopped asks where the cursor goes, nothing plays) | Song mode (bit 16; else Pattern mode) | the pattern id of Pattern mode (bits 8..15). The render writes the time before the word (release), the main loop reads the word
  // first (acquire). The answer is ChaseState + ChaseStart over the sequencer's command ring.
  std::uint32_t chase_request() const noexcept { return chase_req_.load(std::memory_order_acquire); }
  std::uint32_t chase_request_time() const noexcept { return chase_req_time_.load(std::memory_order_relaxed); }
  // MIDI Learn's input (OS-013a): where the control changes of both ports go (midi/cc_in.h). Once, at start-up, before the render runs; the storage is the caller's (the
  // platform keeps it in SDRAM). nullptr detaches it. The render is the ring's one producer, the main loop (App::poll_midi_cc) its one consumer.
  void attach_cc(midi::CcInput* cc) noexcept { cc_ = cc; }

  // ---- the interrupts' ends (the platform's drivers hold these for good) --------------------------------------------------------------
  midi::DinEmitter& din() noexcept { return din_; }
  midi::UsbMidiOut& usb_out() noexcept { return usb_out_; }
  midi::RtInRing& din_in() noexcept { return in_ring_[0]; }
  midi::RtInRing& usb_in() noexcept { return in_ring_[1]; }
  const midi::TimeRef& time_ref() const noexcept { return ref_; }

  // ---- the render ---------------------------------------------------------------------------------------------------------------
  // The engine's event source over this link (audio/event_source.h): its step() is MidiLink::step(); the ParamChange and tempo hooks go to the sequencer.
  audio::EventSource event_source(audio::SeqEvent* buffer, std::uint32_t capacity) noexcept;
  // The engine's live-event hook (EventSource::live; render context, LIVE MIDI NOTES above): true when the event was a pad note-on of a MIDI instrument and has been turned into a
  // MIDI note; an Off or AllOff of the pads ends the MIDI notes they started and answers false (the engine ends its own voices).
  bool live_event(const audio::LiveEvent& e) noexcept;
  // The engine gives itself back after an export (EventSource::resume; main loop, the render parked): what the rings and the clock loop held is forgotten and the loop starts over.
  void resume() noexcept;
  // OS-EXPNOTE (EventSource::stop_live; main loop, the render parked): the pads that hold a note on a MIDI instrument are released and the offs sent now (the link is not stepped during an export,
  // so they would otherwise sound through it): `*count` pads, the first (lowest pad) as an audio::held word (source pad, the note, the channel 1..16), audio::kHeldNone when none.
  std::uint32_t stop_live_pads(std::uint32_t* count) noexcept;
  // One block: the MIDI side of the block that starts at `block_start`, then the sequencer's events (see the top of this file).
  std::uint32_t step(std::uint64_t block_start, std::uint32_t n, audio::SeqEvent* out, std::uint32_t cap) noexcept;

  // ---- reading, for diag and tests ----------------------------------------------------------------------------------------------
  const midi::MidiOut& out() const noexcept { return out_; }
  midi::MidiOut& out() noexcept { return out_; }       // the export slice's set_suppress
  const midi::MidiIn& in() const noexcept { return in_; }
  const midi::ClockDll& dll() const noexcept { return dll_; }
  const midi::mtc::Chaser& chaser() const noexcept { return chaser_; }
  Sequencer& sequencer() noexcept { return seq_; }

  struct Stats {
    std::uint32_t blocks = 0;
    std::uint32_t configs = 0;        // configurations applied
    std::uint32_t stale_releases = 0; // the master went quiet while followed
    std::uint32_t follow_attached = 0;
    std::uint32_t rings_discarded = 0;  // bytes thrown away when the clock-in was switched on (what piled up before)
    std::uint32_t passed_dropped = 0;   // bytes that were not timing (channel messages, active sensing, SysEx): control changes go on to attach_cc's parser, the rest ends here
    std::uint32_t events_applied = 0;
    std::uint32_t transport_ignored = 0;  // E-CFG-03: Start, Continue, Stop and SPP from the followed port that TRANSPORT IN Off dropped
    std::uint32_t chase_requests = 0;     // OS-MTC B3: questions asked of the main loop
    std::uint32_t chase_starts = 0;       // runs the chase began
    std::uint32_t chase_retries = 0;      // answers that came too late (the start sample was past) or for another mapping, and requests that went unanswered for 500 ms
    std::uint32_t chase_relocations = 0;  // relocations and stops of the master that ended a chase
    std::uint32_t chase_locates = 0;      // full frames while stopped that moved the cursor
    std::uint32_t kicks = 0;
  };
  const Stats& stats() const noexcept { return stats_; }

 private:
  void begin_block(std::uint64_t block_start, std::uint32_t heard) noexcept;
  void apply_config(const midi::MidiConfig& c, std::uint64_t block_start) noexcept;
  void drain_inputs() noexcept;
  void discard_inputs() noexcept;
  void manage_follow(std::uint64_t block_start) noexcept;
  void release_follow() noexcept;
  void reset_loop(std::uint64_t origin) noexcept;
  void publish_status(std::uint64_t block_start) noexcept;
  // OS-MTC B3 (render context, once a block, after the inputs): see the file header.
  void manage_chase(std::uint64_t block_start) noexcept;
  void chase_notice(midi::mtc::Chaser::Notice n) noexcept;
  void chase_ask(std::uint64_t block_start, bool locate) noexcept;
  void chase_answered(std::uint64_t block_start) noexcept;
  void chase_settling(std::uint64_t block_start) noexcept;
  void chase_measure() noexcept;
  void chase_reset() noexcept;
  core::Q32 mtc_start_q32() const noexcept;
  std::uint32_t chase_rate_q16() const noexcept;
  void live_release(std::uint32_t pad) noexcept;
  bool live_bend_send(std::uint32_t word, std::uint16_t value) noexcept;  // OS-PIANOBEND: true when the queue took it
  bool live_bend_centre() noexcept;
  std::int32_t in_offset_us() const noexcept { return midi::offset_us(cfg_.in_offset_dms[static_cast<unsigned>(cfg_.in_port)]); }

  Sequencer& seq_;
  MidiLinkHooks hooks_;

  midi::MidiOut out_;
  midi::DinEmitter din_;
  midi::UsbMidiOut usb_out_;
  midi::RtInRing in_ring_[midi::kPortCount];
  midi::MidiIn in_;
  midi::ClockDll dll_;
  midi::ClockMap map_;
  midi::TimeRef ref_;
  midi::mtc::Chaser chaser_;         // OS-MTC B3: the time code of MTC IN's port (about 0.5 KiB: a second ClockDll and the assembler)

  // The settings: the main loop writes the box, the render keeps its own applied copy.
  midi::SeqLockBox<midi::MidiConfig> box_;
  std::uint32_t box_seen_ = 0;
  midi::MidiConfig cfg_{};
  midi::CcInput* cc_ = nullptr;      // MIDI Learn's input (attach_cc), written once before the render starts

  // Render-only state.
  bool want_release_ = false;        // let go of the master (mode or port changed, or it went quiet): stop, then detach the loop
  bool stop_requested_ = false;      // the stop of that release has been asked for; waits for the sequencer to stop
  std::uint64_t last_block_end_ = 0;
  // The MIDI notes the pads hold (live_event; render only): bit 31 = held, bits 25..24 the ports (audio::kRoutePorts), bits 22..16 the OFFSET in ms, bits 11..8 the channel 0..15, bits 6..0 the note, indexed by
  // pad (audio::kLiveSourcePad's key 0..47). 192 bytes. The route is the one the note-on took (OS-INSTUI S8b), so its off follows it whatever the instrument says by then.
  static constexpr std::uint32_t kLivePads = 48;
  static constexpr std::uint32_t kLiveHeld = 0x80000000u;
  static constexpr unsigned kLivePortsShift = 24;
  static constexpr unsigned kLiveDelayShift = 16;
  std::uint32_t live_pad_[kLivePads] = {};
  std::uint32_t bend_route_ = 0;     // OS-PIANOBEND (live_event, render only): the route (live_pad_'s bits, no note) the pad bend strip last bent, kLiveHeld set while it is bent; the centre goes to it on release
  std::uint32_t handed_ = 0;         // MidiOut's handed counter at the last block: a change means the ports have something new
  Stats stats_{};

  // Published for the main loop (render writes, main reads): see SyncStatus.
  std::atomic<std::uint32_t> st_flags_{0};
  std::atomic<std::uint32_t> st_bpm_{0};
  std::atomic<std::uint32_t> st_clocks_{0};
  std::atomic<std::uint32_t> st_error_us_{0};
  std::atomic<std::uint32_t> st_transport_{0};
  // OS-MTC B3: the chase's status (render writes, main reads) and its request words.
  std::atomic<std::uint32_t> st_mtc_{0};        // MtcState | rate code << 8
  std::atomic<std::uint32_t> st_mtc_time_{0};   // hh << 24 | mm << 16 | ss << 8 | ff
  std::atomic<std::int32_t> st_mtc_ppm_{0};
  std::atomic<std::int32_t> st_mtc_err_us_{0};
  std::atomic<std::uint32_t> st_mtc_rel_{0};
  std::atomic<std::uint32_t> chase_req_{0};
  std::atomic<std::uint32_t> chase_req_time_{0};
  enum class ChasePhase : std::uint8_t { Wait, Asked, Settling, Running, PastEnd };
  ChasePhase chase_ph_ = ChasePhase::Wait;
  bool chase_loc_ = false;                // the request in flight is a locate
  bool chase_good_ = false;               // |e| was below 1 ms at the last steps
  std::uint8_t chase_stamp_ = 0;          // the last request's stamp (1..255)
  PlayMode chase_mode_ = PlayMode::Song;  // the mode the request in flight was made for (the sequencer's selection)
  std::uint8_t chase_pattern_ = 0;
  std::uint32_t chase_epoch_ = 0;         // the chaser's mapping the request was made from
  std::uint32_t chase_margin_ = 0;        // samples ahead of now that the next request looks (50 ms, doubled on every retry)
  std::uint32_t chase_seen_ = 0;          // Sequencer::chase_steps() at the last measure
  std::uint64_t chase_asked_at_ = 0;
  std::uint64_t chase_good_since_ = 0;
  // The start in flight: the answer's target (the step's song time plus MTC START) and the start sample the current mapping gives it, watched until it stops moving (a fit that
  // is still settling, as right after a relocation, would put the start off its map time; the rate trim pulls it in over the next second, but the first steps land off).
  core::Q32 chase_pend_target_ = 0;
  std::uint64_t chase_pend_at_ = 0;
  std::uint64_t chase_pend_since_ = 0;
  std::uint32_t chase_frames_ = 0;       // quarter frames read since the last relocation/reset
  bool chase_relocated_ = false;          // the master moved since the last start: the fit needs a hundred frames to converge again
  std::uint64_t chase_first_ask_ = 0;     // the first non-locate question after the last start/reset (0: none yet; locate asks must not anchor it)
  std::uint64_t chase_reloc_at_ = 0;      // the block of the last relocation notice (0: none): the settle max hangs off it, not off later re-asks
};

// The device settings that drive the link (model/params.h device_setting; OS-013a joined the MIDI page's rows of OS-011a to the drivers): CLOCK IN (Internal, USB, MIDI In
// jack; the choices the official manual lists, official-manual-map) and CLOCK MODE (Off, Master, Slave; new) give the mode: an external CLOCK IN is a Slave on that port (the older setting is the more specific one, so a
// settings file from before CLOCK MODE keeps following its clock), otherwise CLOCK MODE decides: Off is Off, Master is a Master, Slave follows the first port of MIDI IN (DIN
// before USB). MIDI OUT (DIN, USB, DIN + USB) is where clock, transport and the notes of MIDI tracks go (out_ports and note_ports), OUT CHANNEL (1..16) the channel of a MIDI
// event that has none (notes_channel), CLOCK SYNC the PAR-119 pulses. MIDI IN and IN CHANNEL are NOT here: they filter the control changes of MIDI Learn, in the main loop
// (app/midi_input.h), from the settings themselves. Every other field is `base`'s (the runtime's own: the offsets, FOLLOW, the CC numbers).
midi::MidiConfig sync_config_from_settings(const model::Settings& device, const midi::MidiConfig& base) noexcept;

// The project's MIDI part (OS-004d, model/project_state.h; docs/formats/o3-project.md "The project-level state"): the fields of MidiConfig that a project stores (CLOCK OUT per port, the
// notes' ports and channel, FOLLOW, the CC numbers, the offsets), and the way back. `mode`, `in_port` and `sync_correction` are the device's and are not in it. apply_project_midi() is
// what a project load does AFTER sync_config_from_settings (so a project that has the part wins until a device setting changes again); both sanitise.
model::MidiState project_midi_state(const midi::MidiConfig& cfg) noexcept;
midi::MidiConfig apply_project_midi(const midi::MidiConfig& base, const model::MidiState& state) noexcept;

}  // namespace o3os::seq
