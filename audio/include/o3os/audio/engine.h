// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The render loop (OS-005a, ADR 0013 D1, D6, D8) and the engine that owns it: the hal::audio::BlockCallback the pipeline calls once per block, at
// the render priority. What one block does, in order, with cycle stamps per stage (hal/audio.h kStage*):
//
//   input    the RX block to planes, its raw peak (the one thing the RX path reported before OS-005b)
//   drain    pop the ParamChange ring, a bounded number per block (ADR 0013 D6: after the block's voices have rendered, in the graph's render, so an edit lands from the next block: a note in the block of a knob edit starts on the old value), apply, retarget the gains
//   render   the sources: the voices (OS-005d lite, below) or the test sources, converted to Q15 once per block at the voice-to-track point (D2)
//   strips   inserts, sends, level, pan, the routing sums into the buses
//   buses    send effects and returns, every bus saturated once
//   master   master chain, master level, cue level, OUT MODE
//   convert  the planar output block to the interleaved TX layout
//   taps     meters, scope, spectrum, stems, REC
//
// All of that is the fixed graph (audio/graph.h, OS-005b); this class is the HW-A harness around it: the single-shot stall hook, the test tone
// script, and the BlockCallback. Before OS-005b the same class made the skeleton's silence and its tone itself; the tone is now two test sources
// of the graph (track 1 hard left, track 2 hard right, both to MAIN at unity), so HW-A also exercises the graph, and at unity the block is bit for
// bit the one the skeleton made.
//
// The voices (OS-005d lite; audio/voices.h). With a VoicePool attached, the first thing a block does is ask the event source (the sequencer, through
// audio/event_source.h: step() once per block, on a uint64 sample clock the engine counts) for the events of the block, and play them: the voices
// render up to each event's sample offset, the event starts, stops or cuts its note, and the rest of the block follows, so a note begins at its exact
// sample. The live notes of the main loop (attach_live) are taken first, at sample 0. The voices' blocks are the tracks' feeds (SourceKind::Feed, one voice a track); the graph does the rest. Voice cycles are added to the Render
// stage of the report, the event source's to the Drain stage.
//
// Context. render_block() runs in the render context only. params() is the producer side of an SPSC ring: exactly one context (the UI / main
// loop) may push, the render context pops (core::SpscRing's contract). set_tone() is called from the main loop (it is the ring's producer) and
// read by the render: one atomic flag, one writer.
//
// OS-WAVEXP, offline rendering. The export renders the same graph, voices and modules from the main loop, faster than real time, while the live render stands down (writes silence and
// touches nothing): see "OFFLINE RENDERING" below for the ownership protocol. The block it renders is the one a live Play would, and nothing else is added to it.
//
// Real-time rules: no heap, no blocking, bounded loops, no double, no printf.

#include <o3os/audio/amiga_voices.h>
#include <o3os/audio/event_source.h>
#include <o3os/audio/graph.h>
#include <o3os/audio/hot.h>
#include <o3os/audio/live_notes.h>
#include <o3os/audio/module_voices.h>
#include <o3os/audio/test_signal.h>
#include <o3os/audio/tracker_fx.h>
#include <o3os/audio/voices.h>
#include <o3os/hal/audio.h>

#include <atomic>
#include <cstdint>

namespace o3os::audio {

class Engine {
 public:
  // `taps` is the taps' memory (tap_mem.h), placed by the caller (the MK66 image: SRAM_L); the graph resets it.
  Engine(CycleFn cycles, TapMem& taps) noexcept;

  // The ParamChange ring: producer side for the UI (one context), consumer side is the render.
  ParamRing& params() noexcept { return graph_.params(); }
  // The graph: its taps (meters, scope, spectrum, stems, REC) and its configuration (before start).
  Graph& graph() noexcept { return graph_; }
  const Graph& graph() const noexcept { return graph_; }

  // The voices and their events (OS-005d), before start (or with the render stopped; the platform attaches them when the document exists, ahead of
  // hal::audio::Device::start()). Attaching the voices makes every track's input its voice block (Graph::use_feed_sources); the event source is
  // optional (a test drives the pool by hand) and also takes the ParamChanges the graph does not own (Graph::set_param_hook).
  void attach_voices(VoicePool* voices) noexcept;
  void attach_events(const EventSource& source) noexcept;
  VoicePool* voices() noexcept { return voices_; }
  // The live notes (OS-005g; audio/live_notes.h has the queue contract): the main loop's pads, MIDI keyboard and previews, taken at the start of every block, before the
  // event source's events. The box (the queue and the table of held notes) is the caller's (SDRAM on the MK66); the main loop is the queue's one producer, the render (this
  // class) its one consumer and the table's only user. Before start. live_notes() is for tests and `diag`: the held notes (null table: none).
  void attach_live(LiveBox* box) noexcept { live_ = box; }
  unsigned live_held() const noexcept { return live_ != nullptr ? live_->notes.held() : 0u; }
  // MAIN LOOP: true when no live note can start or is held: the queue is empty (the main loop is the only producer, so nothing arrives while it asks) and the render's last
  // look at the table found no held note. The render pops and acts on an event inside one block, publishes the held count at the end of play_live(), and preempts the main
  // loop but never the other way round, so "empty, then published count 0", read in this order, means every event pushed so far has been acted on and none is held.
  // The sample bank's handover asks this (app::PreviewSink::live_idle) before it rewrites a descriptor a live note would read (OS-011a on OS-005g).
  bool live_idle() const noexcept { return live_ == nullptr || (live_->queue.empty() && live_held_.load(std::memory_order_acquire) == 0u); }
  // OS-EXPNOTE: what the export stopped when it took the engine (offline_enter): the oldest held live note as a word (audio::held_source() and the other accessors of live_notes.h;
  // kHeldNone: none) and how many were held. Main loop.
  std::uint32_t live_stopped_what() const noexcept { return live_ != nullptr ? live_->stopped_what : kHeldNone; }
  unsigned live_stopped_count() const noexcept { return live_ != nullptr ? live_->stopped_count : 0u; }
  // OS-012a: the instruments that are SD modules (audio/module_voices.h). Optional (null: none): the note events of a slot bound there go to it instead of
  // the sampler voices, and its block is the track's feed. Main loop, before the audio starts or with the render idle; the pointer is read by the render.
  // Attaching them also switches on the spare strip (kAuditionTrack, graph.h) that `diag mod note` plays on (OS-MODSILENT).
  void attach_modules(ModuleVoices* modules) noexcept;
  ModuleVoices* modules() noexcept { return modules_; }
  // OS-005h: the instruments whose playback engine is Amiga (audio/amiga_voices.h). Optional (null: none; SampleDesc::engine is then ignored and the note plays Modern): the
  // events of those instruments and the Tick events go to it, and its block is the track's feed. Main loop, before the audio starts or with the render idle.
  void attach_amiga(AmigaVoices* amiga) noexcept { amiga_ = amiga; }
  AmigaVoices* amiga() noexcept { return amiga_; }
  // The samples of the timeline so far: the block start the next step() call is given (render context; a test reads it between blocks).
  std::uint64_t sample_clock() const noexcept { return clock_; }

  // The tone is off until asked for. Main loop writes, render reads; the gain ramps over kToneRampSamples either way. Switching it on first puts
  // the script's strip settings (tracks 1 and 2 at unity, panned hard left and hard right, to MAIN, not muted) on the ParamChange ring as instant
  // changes, then starts the script: left alone for `left_only_blocks` blocks (kToneLeftOnlyBlocks), then both channels; switching it off resets
  // it. False (and nothing started) if the ring had no room for the settings.
  bool set_tone(bool on) noexcept;
  bool tone_requested() const noexcept { return tone_target_.load(std::memory_order_relaxed); }
  // Before start (tests use a short phase). Not a real-time control.
  void set_left_only_blocks(std::uint32_t blocks) noexcept { left_only_blocks_.store(blocks, std::memory_order_relaxed); }

  // TEST HOOK, single shot (the HW-A "forced underrun"): the next render burns `cycles` of the platform's cycle counter before it
  // starts, so with `cycles` above a block period the boundaries pass inside it and the counters, the mute and the recovery can be
  // seen on hardware. Capped at kMaxTestStallCycles and bounded by an iteration limit. Main loop writes, the render consumes.
  static constexpr std::uint32_t kMaxTestStallCycles = 4u * hal::audio::kCyclesPerBlock;
  void request_test_stall(std::uint32_t cycles) noexcept { stall_cycles_.store(cycles < kMaxTestStallCycles ? cycles : kMaxTestStallCycles, std::memory_order_relaxed); }

  // TEST HOOK (OS-ODD11): the tracks the voice gate (VoicePool GATE) may act on; 0 turns it off, so a test can render the same scene with and without it.
  void set_voice_gate_mask(std::uint16_t mask) noexcept { gate_off_ = static_cast<std::uint16_t>(~mask); }

  // The hal::audio::BlockCallback of this engine.
  hal::audio::BlockCallback callback() noexcept { return hal::audio::BlockCallback{&Engine::trampoline, this}; }

  // ---- OFFLINE RENDERING (OS-WAVEXP) -----------------------------------------------------------------------------------------------------------------------------
  // OWNERSHIP. The graph, the voices, the modules, the Amiga voices and the sequencer behind the event source belong to ONE renderer at a time: the live render (callback() into render_block, the render
  // line) or, between offline_enter() and offline_leave(), the export (render_offline, the main loop). They never render at the same time; the protocol:
  //   1. offline_request()      main loop. Clears `parked_`, then sets `want_` (release). From the live render's next block start on, the callback (trampoline) writes a block of digital silence, renders
  //                             nothing, and sets `parked_` (release): it has stood down. The render line is above the main loop and runs a block to completion, so a block that began before the
  //                             request has ended by the time the main loop runs again, and every later block parks: the main loop sees `want_` and no live block in progress.
  //   2. offline_ready(live)    main loop, polled each pass (never waits): `parked_` seen (acquire), or `live` false (the audio is not running: a host test, a boot with no audio).
  //   3. offline_enter(source)  main loop, once ready: the event source and the sample clock are swapped for the export's (restored by 5; the sample clock starts at 0, as a boot does), the USB stems tap,
  //                             the apps' tap, the scope and the spectrum, and the input strip's feed are put away (the host and the apps must not hear an export, and the codec's line-in and the USB return are
  //                             not in it), the tracker FX processor is reset, the module governor is switched off, the Perform layer is put away and cleared in the voices and the strips (OS-PERFFX P0: the
  //                             export's sequencer has none, and a pad held in Master or Song must not colour it), and every voice, module note and Amiga voice is stopped. ParamChanges the graph does not own go to
  //                             the export's source AND the live one. The ParamChange ring stays the graph's own: whatever the main loop pushed is applied by the first offline block, as a live block would.
  //   4. render_offline(io)     main loop, any number of times: one block into io.out from the silent io.in the caller gives. The live notes' queue is NOT read (what was queued before 3 is dropped there, an off pushed meanwhile waits for 5; the notes held at 3 were
  //                             stopped there: stop_live_notes, OS-EXPNOTE), nothing of the HW-A script runs, and no stage cycles are reported.
  //   5. offline_leave()        main loop: restores what 3 swapped (the Perform layer too), tells the live source to forget what it kept (EventSource::resume: the MIDI link drops the bytes the interrupts stamped meanwhile and starts its clock loop
  //                             over), stops every voice again, then clears `want_` (and `parked_`): the live render takes the engine back at its next block start with
  //                             the voices silent. The send effects' memory is the caller's to clear (the DelayClear and ReverbClear ParamChanges), before and after, if it wants a start from quiet.
  // `want_` is written by the main loop and read by the render, `parked_` the other way round; everything 3 to 5 touch is the main loop's while `want_` is set. The cost on the live path is one atomic load, in flash (the trampoline), none in the hot render.
  void offline_request() noexcept;
  bool offline_ready(bool live_running) const noexcept;
  void offline_enter(const EventSource& source) noexcept;
  void offline_leave() noexcept;
  bool offline_wanted() const noexcept { return want_.load(std::memory_order_relaxed); }
  bool offline_entered() const noexcept { return offline_entered_; }
  // Main loop, between offline_enter() and offline_leave(). `io.in` is a block of silence (kBlockSamples), `io.out` the block; `io.report` may be a scratch report.
  O3OS_AUDIO_COLD void render_offline(const hal::audio::BlockIo& io) noexcept;
  // Main loop, while the engine is the export's (offline_enter): every voice, module note and Amiga voice stops at once (the residual of a sampler voice fades within a block).
  O3OS_AUDIO_COLD void offline_quiet() noexcept;

  // One block. `io.report` is filled with the stage cycles (Input..Taps; the commit stage is the pipeline's). The live render's entry is callback() (trampoline): it hands the block to the export's park (below) when
  // the export has the engine, and to this otherwise; calling this directly is the live render with no export to ask (the benches and most tests).
  O3OS_AUDIO_HOT_FLAT void render_block(const hal::audio::BlockIo& io) noexcept;  // (flattened: the atomic loads of the tone script are inlined, SRAM_L calls nothing in flash)

  // Diagnostics (single words; the render is the only writer).
  std::uint32_t params_applied() const noexcept { return graph_.params_applied(); }
  const TestTone& tone_left() const noexcept { return graph_.tone(0); }
  const TestTone& tone_right() const noexcept { return graph_.tone(1); }

 private:
  // The BlockCallback's function, in flash (OS-WAVEXP): the export's request is looked at here, so that the hot render in SRAM_L (.ramfunc_l has no bytes to spare) does not carry the check: a block of digital
  // silence and `parked_` when the export has asked for the engine, render_block() when it has not. One acquire load of `want_`.
  static void trampoline(void* user, const hal::audio::BlockIo& io) noexcept;
  O3OS_AUDIO_HOT void play_block() noexcept;
  O3OS_AUDIO_HOT void dispatch(const SeqEvent& e) noexcept;
  O3OS_AUDIO_COLD void module_fx(const SeqEvent& e) noexcept;  // OS-MODPITCH: the Slide and Glide FX (and the reset) for the module instruments, in flash: dispatch() keeps one call a site
  O3OS_AUDIO_COLD void tracker_event(const SeqEvent& e) noexcept;  // OS-COREFX: the tracker FX processor's events (TrackerFx::takes), in flash: dispatch() keeps one call
  // The export's ParamChange hook (main loop, while the engine is the export's): the export's sequencer takes the change, and so does the live one, which is parked and would otherwise never see a
  // change that was on the ring when the export began (the render has no other way to hand it over). True when either took it.
  static bool offline_param(void* self, const ParamChange& change) noexcept;
  O3OS_AUDIO_COLD void apply_pan_mods() noexcept;  // OS-MOD: the autopan's per-block pan to the graph (flash)
  // OS-BUG-MODDRIVE: Overdrive / Bit depth (step FX 38 / 39, Perform OVERDRV / BITDEPTH) on the module instruments' blocks, in place, between the modules' end_block() and the feed. `mask`: VoicePool::shape_mask()
  // (non-zero, the caller's test). In flash, once a block while any track holds either FX, plus one shape_pcm per shaped module track (hot.h exceptions).
  O3OS_AUDIO_COLD void shape_modules(std::uint16_t mask) noexcept;
  // OS-CPU10: flash (1,468 B of SRAM_L back). play_block() makes the empty-queue test inline and calls this only when the main loop has queued a live event (a pad press).
  O3OS_AUDIO_COLD void play_live() noexcept;
  O3OS_AUDIO_COLD bool live_amiga_on(unsigned track, unsigned note, unsigned slot, unsigned velocity) noexcept;
  // OS-LIVEP: the graph's hook for a ParamChange of kind Instrument (Graph::set_instrument_hook), in flash and reached through a pointer, once per change drained: the sampler voices'
  // descriptors and the voices that play them take it (VoicePool::apply_param), and a pan the voices have to give goes to their tracks with one block of smoothing.
  static bool instrument_hook(void* self, const ParamChange& change) noexcept;
  O3OS_AUDIO_COLD void stop_live_notes() noexcept;  // OS-EXPNOTE: offline_enter() ends the held live notes (the table forgets them, the queue is emptied, the MIDI link's pads are released) and keeps what it stopped

  CycleFn cycles_;
  Graph graph_;
  VoicePool* voices_ = nullptr;
  LiveBox* live_ = nullptr;           // OS-005g: set before start
  ModuleVoices* modules_ = nullptr;  // OS-012a
  AmigaVoices* amiga_ = nullptr;     // OS-005h
  TrackerFx tfx_;                    // OS-COREFX: the tracker FX of the sampler voices (audio/tracker_fx.h), run on the TrkFx and Tick events from dispatch()
  std::atomic<bool> want_{false};    // OS-WAVEXP: the export has asked for the engine (main loop writes, the render reads); see OFFLINE RENDERING
  std::atomic<bool> parked_{false};  // ... and the render has stood down (the render writes, the main loop reads)
  bool offline_entered_ = false;     // main loop only: between offline_enter() and offline_leave()
  bool stems_were_on_ = false;       // main loop only: what offline_enter() put away
  bool governed_was_ = false;        // ... the module governor's switch (ModuleVoices::set_governed)
  InputFeed feed_was_ = InputFeed::Sum;
  EventSource live_source_;          // main loop only: the live event source, restored by offline_leave()
  std::uint64_t live_clock_ = 0;
  std::uint32_t tap_was_ = 0;        // main loop only, between offline_enter() and offline_leave(): the apps' tap source and the scope and spectrum switches that were put away (an export is heard by none)
  bool scope_was_ = false;
  bool spectrum_was_ = false;
  std::atomic<std::uint8_t> live_held_{0};  // render writes (end of play_live, when it took an event), main loop reads: live notes held (kLiveMax, 12)
  EventSource source_;
  std::uint64_t clock_ = 0;           // render only: the first sample of the next block on the sequencer's timeline
  std::uint32_t fx_tempo_ = 0;        // render only: the tempo (BPM x 100) the send effects were last told (OS-SENDFX); 0: none yet
  std::uint32_t seq_cycles_ = 0;      // render only: this block's cycles in the event source, and in the voices (added to the report)
  std::uint32_t voice_cycles_ = 0;
  std::uint16_t gate_off_ = 0;         // OS-ODD11: the tracks the gate may not act on (set_voice_gate_mask); zero, so the Engine stays in .bss
  std::atomic<bool> tone_target_{false};
  std::atomic<std::uint32_t> left_only_blocks_{kToneLeftOnlyBlocks};
  std::atomic<std::uint32_t> stall_cycles_{0};
  std::uint32_t blocks_on_ = 0;  // render only: blocks since the tone was switched on (the script's clock)
  bool script_left_ = false;     // render only: the script's last command to each tone (it speaks on a change only)
  bool script_right_ = false;
  VoicePool::PerfSave perf_voices_{};  // OS-PERFFX P0, main loop only, between offline_enter() and offline_leave(): the Perform layer the export put away (the voices' and the strips')
  Graph::PerfSave perf_strips_{};
};

}  // namespace o3os::audio
