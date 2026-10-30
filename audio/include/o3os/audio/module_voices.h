// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The instruments that are SD modules (OS-012a, ADR 0002): a module bound to an instrument slot plays on a track like a sample instrument, through the
// same graph. The Engine hands it the note events of its slots (audio/engine.h) and takes the track's block from it where a sample voice would be.
//
//   ONE INSTANCE A SLOT: bind() gives the instrument its own instance of the module (the state block and the init() call are the caller's, the main
//   loop's). The instance is mono, as the 303 is: a note on it from another track takes the instance over (the first track's block goes silent).
//   A NEW NOTE IS A NEW NOTE (OS-MODSILENT). A tracker note on a track replaces the track's previous one and no note-off comes first, so "the last note is still held" says nothing about the
//   player's hands: a note that comes from the sequencer (Engine::dispatch) first RELEASES the instance's held note (note_off) and then starts with flags 0, and the module retriggers its
//   envelopes (the same for the pads). The Glide FX of the sampler voices is a pitch ramp only, not a legato gate. What the official firmware's sampler does with its envelopes at a note on is an
//   open research question (Q).
//   O3M_NOTE_LEGATO (a glide, for a module that has one: NEW COMMUNITY OS BEHAVIOUR for module instruments) is passed for a live key (or the audition) pressed
//   while another LIVE key is down: note_on's `overlap` says the note is played by hand, and Instance::held_overlap remembers that the held note was too. A pad struck over a SEQUENCER's note
//   is a new note (the voice still rings, but nobody played it: a slide into it would have no envelope and the pad would be quiet).
//   THE SLIDE AND GLIDE FX ON A MODULE (OS-MODPITCH, ABI 1.8; docs/specs/OS-MODPITCH.md; NEW COMMUNITY OS BEHAVIOUR). A module that exports `pitch` is driven by the host: fx_event() holds the track's Slide
//   (41 / 42) and Glide (19) FX, the pitch offset is a PitchRamp (audio/pitch_ramp.h: the law of the sampler voices) walked once a block, and each step of it is one pitch(state, semitones, per_frame) call.
//   A pattern note under such an FX is LEGATO, as a TB-303 slide: the held note is NOT released first, the module gets note_on(note, O3M_NOTE_LEGATO | O3M_NOTE_RAMP) and then pitch(); with nothing
//   held (a first note, or after a release) just O3M_NOTE_RAMP; a Slide or Glide note on the note that sounds is a tie (nothing at all). A module without `pitch` (NULL, or of ABI 1.7 or below: its
//   64-byte descriptor has no such member, bind() never reads it) is never called and never sees the flag: the FX are ignored for it as before.
//
//   OUTPUT: the module renders float into a scratch block; each segment of the block is converted to Q15 (the integer part of x * 32768, cut towards zero; saturating; a NaN is 0, see module_voices.cpp to_q15) into the instance's own block, which
//   the engine gives the graph as the track's feed. The blocks and the scratch are the caller's memory (attach_scratch: the SRAM_L slab on the MK66, so the
//   audio path never reads SDRAM for them); a block is zeroed the first time it is touched in a block, so a note that starts mid-block is exactly silent
//   before its sample. A module that says it is idle (render() returns 0) is not called again until its next note.
//
// CONTEXTS. Main loop: attach_scratch, set_cycles, bind, retire_all, quiescent, release_all, set_param, audition, voice_cost, muted, the governor's tunables and snapshot. Render context (the audio
// interrupt): begin_block, render, end_block, note_on, note_off, fx_event, set_tempo_x100, release_track, release_notes, feed, bound, the governor's admit / account / observe_block. The main loop never touches an instance the render may be using:
// bind() publishes it after init() has returned (release store); retire_all() withdraws every instance at once (the render stops using one at its next
// block start at the latest), quiescent() says the render has since passed two block starts, and only then may release_all() call shutdown() and the
// memory be reused. set_param() and audition() go through a bounded SPSC ring that the render drains at each block start, so a module's functions are
// only ever called from one context at a time. Nothing here allocates, blocks or loops unboundedly (kMaxInstances, kMaxCommandsPerBlock).
//
// GOVERNOR (OS-GOV, audio/module_governor.h): the render times each instance (a stamp of the cycle counter around its render()), keeps a measured cost per voice, refuses a note that would START a voice the block cannot afford
// (note_on returns false: nothing started and the track's note is released; a retrigger of a sounding instrument adds no voice and is always admitted) and mutes an instance that stays over its time
// budget. Admission is note_on's, in the render context.
// Without a cycle counter (set_cycles never called) there is no measure and no governor.
//
// PERF (OS-303PERF, audio/module_perf.h): the same stamps, five of them around each segment of an instance (before enter(), after it, after render(), after leave(), after the conversion), give the cost of
// the module's render() alone against the whole instance loop. The segment adds three words to the instance (render only); once a block, in account_block() (flash), a FULL block of the instance is folded
// into min / mean / max per bucket. IT IS OFF FROM BOOT: until `diag cpu reset` (reset_perf()) turns it on, the render reads one more flag a segment and nothing else (about 10 instructions an instance and block); `diag cpu
// perf off` (stop_perf()) turns it off again. While it is ON it is not free and it is not invisible to the governor: the three extra counter reads (after enter(), after render(), after leave()) fall INSIDE the
// governor's window (its t0 to its last stamp), so the figure it measures, `diag mod`'s cycles/voice and every cost it admits notes by, is about three counter reads a segment higher than with the split off (the zero-fill's
// read is outside it). Together with the adds and the fold that is about 150 instructions an instance and block (0.1 % of the block [est]: QEMU icount, the governor test prints it, not measured on the unit). A figure for
// comparison with another build's, or the engine's maxima as a baseline, is taken with the split OFF. Read by `diag mod` (perf(), a seqlock; see PerfCell for who writes); `diag cpu reset` starts the figures again (the
// render clears the cells at its next block, the reader shows zeros until then).
//
// No heap. The object (the command ring is 768 B, the instances' table with their perf cells) lives in SRAM_U on the MK66, not in SDRAM (OS-PERF3: the render touches it every block; app_main.cpp g_module_voices).

#include <o3module.h>

#include <o3os/audio/events.h>
#include <o3os/audio/hot.h>
#include <o3os/audio/module_governor.h>
#include <o3os/audio/module_perf.h>
#include <o3os/audio/pitch_ramp.h>
#include <o3os/core/ring.h>
#include <o3os/hal/audio.h>

#include <atomic>
#include <cstdint>

namespace o3os::audio {

inline constexpr unsigned kMaxModuleInstances = 8;
inline constexpr unsigned kModuleSlots = 64;   // the model's kMaxInstruments (a test holds them equal)
inline constexpr unsigned kModuleTracks = 16;  // the graph's kTracks

class ModuleVoices {
 public:
  static constexpr std::uint32_t kBlock = hal::audio::kBlock;
  static constexpr std::uint32_t kCommandRing = 64;
  static constexpr std::uint32_t kMaxCommandsPerBlock = 32;
  static constexpr std::uint8_t kDefaultVelocity = 96;  // a note that names none: under the accent threshold of 100
  // The memory attach_scratch() wants: the float block the module renders into, then one Q15 block for each instance, 16-byte aligned.
  static constexpr std::uint32_t kScratchBytes = kBlock * 4u + kMaxModuleInstances * kBlock * 2u;

  // What the platform does around module code (the crash context of ADR 0002 "Containment": the module that was running when the chip faulted).
  // `enter(id)` before a call into a module with its fault_id (module/loader.h) returns the id that was stamped before (the main loop's own driver, when
  // the render interrupts it), `enter(previous)` puts it back after. May be null. Called from either context.
  // `log(text)` is the module's host->log (init and shutdown only): one line to the device's log; null: dropped.
  struct Hooks {
    std::uint16_t (*enter)(std::uint16_t fault_id) = nullptr;
    void (*log)(const char* text) = nullptr;
  };

  struct Stats {
    std::uint32_t note_ons = 0;   // notes that reached an instance
    std::uint32_t dropped = 0;    // commands the ring could not take (the main loop's count)
    std::uint32_t rendered = 0;   // instance-segments rendered
    std::uint32_t saturated = 0;  // samples that clipped in the Q15 conversion (their integer part left the int16 range; a NaN, which is silence, is not one)
  };

  ModuleVoices() noexcept;
  ModuleVoices(const ModuleVoices&) = delete;
  ModuleVoices& operator=(const ModuleVoices&) = delete;

  // The service table modules are given (for tests).
  const o3m_host& host() const noexcept { return host_; }

  // ---- main loop --------------------------------------------------------------------------------------------------------------------
  // `mem` (at least kScratchBytes, 16-byte aligned) is the scratch and the instances' blocks. False when it is too small or not aligned.
  bool attach_scratch(void* mem, std::uint32_t bytes) noexcept;
  void set_hooks(const Hooks& hooks) noexcept;
  void set_sample_rate(float hz) noexcept { host_.sample_rate = hz; }
  // The cycle counter the governor measures with (the engine's: Engine::attach_modules). Before the audio starts. Null: no measure, no governor.
  void set_cycles(gov::Clock clock) noexcept {
    cycles_ = clock != nullptr ? clock : &no_clock;  // (the render reads it without a test)
    governed_ = clock != nullptr;
  }
  // OS-WAVEXP: whether the instances are measured and the notes judged. The export turns it off for its blocks (main loop, the render parked): its blocks run on the main loop's wall clock, with
  // interrupts inside the windows, so a budget judged there would drop notes or mute an instance, and two passes would differ; the render of an export is as heavy as it is and sounds like a live Play
  // that fits. Restored by the engine when it gives itself back. Never turns a governor on that has no counter.
  bool governed() const noexcept { return governed_; }
  // Switching it back on forgets what the instances added up while it was off (render() adds to the accumulators whatever the switch says, account_block() only clears them when governed): the first
  // block after an export must be measured by itself, not as the sum of every block of the export (which could also wrap a counter).
  void set_governed(bool on) noexcept {
    const bool was = governed_;
    governed_ = on && cycles_ != &no_clock;
    if (governed_ && !was) {
      for (Instance& in : in_) {
        in.gov.acc_cycles = 0;
        in.gov.acc_frames = 0;
        in.acc_render = in.acc_out = in.acc_zero = 0;
      }
    }
  }
  gov::Governor& governor() noexcept { return gov_; }
  const gov::Governor& governor() const noexcept { return gov_; }
  // Main loop, for the pages: the measured cycles of one voice of the instrument's module for a whole block (0: never measured, or not bound), and whether the safety net muted the instance.
  std::uint32_t voice_cost(unsigned slot) const noexcept {
    const std::uint8_t i = slot < kModuleSlots ? slot_[slot].load(std::memory_order_acquire) : kNone;
    return i != kNone ? in_[i].gov.cost.load(std::memory_order_relaxed) : 0u;
  }
  bool muted(unsigned slot) const noexcept {
    const std::uint8_t i = slot < kModuleSlots ? slot_[slot].load(std::memory_order_acquire) : kNone;
    return i != kNone && in_[i].gov.muted.load(std::memory_order_relaxed) != 0u;
  }

  enum class Bind : std::uint8_t { Ok, NoScratch, BadSlot, SlotTaken, NoInstance, BadState, InitFailed };
  // Binds instrument `slot` (0..63) to an instance of `mod`: `state` is `mod.state_bytes` (or more) of zeroed, 16-byte aligned memory, init() is called
  // on it (the module's own failure refuses the bind), then the stored parameter blob (`params`, `param_len` bytes: one little-endian float32 per
  // parameter, in the module's order; a short blob sets the first ones, non-finite values are skipped) is applied, and the instance is published.
  // `fault_id` is what the hooks get while module code runs.
  Bind bind(unsigned slot, const o3m_module& mod, std::uint16_t fault_id, void* state, const std::uint8_t* params, std::uint32_t param_len) noexcept;

  // Withdraws every instance from the render (a project is about to change). Returns at once; quiescent() says when the render is clear of them.
  void retire_all() noexcept;
  // True when the render has passed two block starts since retire_all() (or nothing was bound). The caller that has no render running (a host test, a
  // boot with no audio) calls release_all() without waiting.
  bool quiescent() const noexcept;
  // shutdown() on every retired instance, and forget them. Only when quiescent() or the render is not running.
  void release_all() noexcept;

  O3OS_AUDIO_INLINE bool bound(unsigned slot) const noexcept { return slot < kModuleSlots && slot_[slot].load(std::memory_order_acquire) != kNone; }
  // OS-INSTUI S5: the gate of a bound slot. A module instance cannot be unbound before the next project load, so an instrument deleted from its slot (or the slot given a sample or MIDI instrument)
  // leaves the instance bound; the gate is what keeps it from sounding: the main loop closes it while the slot's instrument is not a Module (ModuleUi::sync_slots) and opens it again when it is
  // (an undo). Single writer (the main loop), one byte per slot, read by the render at every note; routed() is what the engine and the note path ask, bound() is the binding itself. A bind opens it.
  O3OS_AUDIO_INLINE bool routed(unsigned slot) const noexcept { return bound(slot) && gate_[slot].load(std::memory_order_acquire) == 0u; }
  // Closing an open gate also ends what the instance is playing (a queued Release the render serves at its next block start: the notes on its tracks end and the tracks are free); a note
  // queued by audition() before it is dropped by the gate.
  void set_gate(unsigned slot, bool open) noexcept;
  // The state block of the instance bound to `slot` (OS-012b: what a view of an instrument module is given, read only by contract: the render may be changing it), or null.
  void* state_of(unsigned slot) const noexcept {
    if (slot >= kModuleSlots) {
      return nullptr;
    }
    const std::uint8_t i = slot_[slot].load(std::memory_order_acquire);
    return i != kNone ? in_[i].state : nullptr;
  }
  // A parameter change for the module bound to `slot`, applied by the render at its next block start. False when the slot is not bound or the ring is full.
  bool set_param(unsigned slot, unsigned id, float value) noexcept;
  // A note without the sequencer (`diag mod note`): on `track`, at the next block start. `on` false releases the track's note.
  bool audition(unsigned slot, unsigned track, unsigned note, unsigned velocity, bool on) noexcept;

  // For `diag` and the tests (main loop; the render writes these a word at a time).
  const Stats& stats() const noexcept { return stats_; }
  unsigned instances() const noexcept;
  // Slot, name and state of instance i (0..kMaxModuleInstances-1) as the last block left it: false when i is free.
  struct Info {
    unsigned slot;
    const o3m_module* mod;
    bool held;
    bool active;
    unsigned track;  // kNoTrack when none
    std::uint32_t cost;  // OS-GOV: the measured cycles of one voice for a whole block, 0 never measured
    bool muted;          // OS-GOV: the safety net muted the instance
    const void* state;   // OS-303PERF: the instance's state block (where it lives says SRAM_U or SDRAM: `diag mod`)
  };
  bool info(unsigned i, Info& out) const noexcept;
  // OS-303PERF: the render's own measure of instance i (0..kMaxModuleInstances-1) over the full blocks since its bind or the last reset_perf(), see module_perf.h. False when i is free, or the
  // render was writing it all four times the reader looked (try again); a reset not yet seen by the render reads as no block. Any context.
  bool perf(unsigned i, ModulePerf& out) const noexcept;
  // Turns the measure on (it is off from boot: the render does not read the extra stamps) and starts every instance's figures again (`diag cpu reset`): a request the render serves at its next block that
  // sounded an instance. While it is on, the governor's own figure carries the three extra counter reads of each segment (see PERF above). Main loop.
  void reset_perf() noexcept {
    perf_on_.store(1u, std::memory_order_relaxed);
    perf_reset_.fetch_add(1u, std::memory_order_release);
  }
  // Turns it off (`diag cpu perf off`): from the next block the render reads no extra stamps and folds nothing, so the governor's figure is the one it was before OS-303PERF. The cells keep their figures (perf() still reads them; a reset
  // that was waiting is still served), but `diag mod` lists no perf line while it is off: read them first. reset_perf() clears them and turns it on again. Main loop.
  void stop_perf() noexcept { perf_on_.store(0u, std::memory_order_relaxed); }
  bool perf_enabled() const noexcept { return perf_on_.load(std::memory_order_relaxed) != 0u; }

  // ---- render context ---------------------------------------------------------------------------------------------------------------
  // One block is: begin_block(); then in time order render(begin, at) and the event's call; render(cursor, kBlock); end_block().
  O3OS_AUDIO_HOT void begin_block() noexcept;
  O3OS_AUDIO_HOT void render(std::uint32_t begin, std::uint32_t end) noexcept;
  O3OS_AUDIO_HOT void end_block() noexcept;
  // OS-GOV, once a block after the whole render (the engine): the cycles of the block's stages (Input..Taps; the engine's report, the pipeline adds the commit) feed the recent worst render. In flash.
  O3OS_AUDIO_COLD void block_done(const std::uint32_t (&stage_cycles)[hal::audio::kStageCount]) noexcept;
  // The note of a NoteOn event for `slot` (`velocity` 0..127 or kVelocityDefault). False: the slot is not a module's (the engine plays it as a sample), or the governor refused the note (OS-GOV:
  // dropped for lack of room, or the instance is muted): nothing started either way (and the track's previous module note is released), and the engine does not play a sample for a slot that is bound.
  // `overlap`: the note is played by hand (a live key, the audition). Over a held note that was played by hand too it is legato: O3M_NOTE_LEGATO and no note_off in between. Otherwise (the
  // sequencer's notes, or a key over a sequencer's note) a held note is released first and the new one starts as a new note.
  O3OS_AUDIO_HOT bool note_on(unsigned track, unsigned note, unsigned slot, unsigned velocity, bool overlap = false) noexcept;
  O3OS_AUDIO_HOT void note_off(unsigned track) noexcept;       // the track's note ends (a NoteOff, NoteCut or NoteFade event)
  O3OS_AUDIO_HOT void release_track(unsigned track) noexcept;  // a sample note starts on the track: the module's note ends and its block is no longer the track's
  O3OS_AUDIO_HOT void release_notes() noexcept;                // the transport stopped: every held note ends

  // OS-MODPITCH (ABI 1.8), render context, in flash: a ModSet / ModEnd of the Slide or Glide group, or an FxReset, from Engine::dispatch (SeqEvent kinds ModSet, ModEnd, FxReset; any other is ignored).
  // Holds the FX per track; a Slide FX also moves the module sounding on the track from where it is.
  O3OS_AUDIO_COLD void fx_event(const SeqEvent& e) noexcept;
  // The tempo in force (BPM x 100, the engine once a block, as the voices'): a Slide FX lasts one step of it, read when the ramp starts. 120 BPM until told.
  O3OS_AUDIO_INLINE void set_tempo_x100(std::uint32_t x100) noexcept { tempo_x100_ = x100; }

  // A module's note is held or sounding on the track (render context): the live notes' track search treats it as a playing voice (Engine::play_live).
  O3OS_AUDIO_INLINE bool track_busy(unsigned track) const noexcept {
    if (track >= kModuleTracks) {
      return false;
    }
    const std::uint8_t i = owner_[track];
    return i != kNone && in_[i].track == track && (in_[i].held | in_[i].active) != 0u;
  }

  // The track's block of this block (kBlock samples, 4-byte aligned), valid until the next begin_block(); null when no instance sounded on the track.
  O3OS_AUDIO_INLINE const std::int16_t* feed(unsigned track) const noexcept {
    if (track >= kModuleTracks) {
      return nullptr;
    }
    const std::uint8_t i = owner_[track];
    return i != kNone && in_[i].track == track && ((touched_ >> i) & 1u) != 0u ? block_[i] : nullptr;
  }
  // OS-BUG-MODDRIVE: the same block to change in place (Engine::shape_modules, between end_block() and the engine's feed): the same selection as feed(), one instance a track, so no block is reached twice.
  O3OS_AUDIO_INLINE std::int16_t* feed_mut(unsigned track) noexcept {
    if (track >= kModuleTracks) {
      return nullptr;
    }
    const std::uint8_t i = owner_[track];
    return i != kNone && in_[i].track == track && ((touched_ >> i) & 1u) != 0u ? block_[i] : nullptr;
  }

 private:
  static constexpr std::uint8_t kNone = 0xFF;
  enum class Cmd : std::uint8_t { SetParam, NoteOn, NoteOff, Release };
  struct Command {
    Cmd kind;
    std::uint8_t slot;
    std::uint8_t track;
    std::uint8_t velocity;
    std::uint16_t a;  // parameter id / note
    std::uint16_t pad;
    float value;
  };
  static_assert(sizeof(Command) == 12, "a command is three words");
  // A ModulePerf with a reader in any context and two kinds of writer: the render (add() once a block, and clear() for every cell when `diag cpu reset` asked, whether the instance is bound or not) and the main
  // loop's bind() (clear() of the cell of an instance it is about to publish). The two can meet on one cell (the render preempts bind() after its first store of the sequence number), which is harmless
  // because clear() only writes zeros and the minima's start value, whichever finishes last leaves the same words, and each clear() ends with an even sequence number (the render's clear nests inside bind()'s and finishes first;
  // bind()'s last store then sets it even again). add() runs only in the render, and only on a published instance, so it never meets a clear() of bind(). The words are relaxed atomics (a load and a store, no more)
  // between two reads of a sequence number that is odd while a writer is in the middle (a seqlock: the reader retries, and gives up after four tries). The writer's side is in flash and runs once a block.
  class PerfCell {
   public:
    void clear() noexcept;
    void add(std::uint32_t loop, std::uint32_t render, std::uint32_t out) noexcept;
    bool read(ModulePerf& out) const noexcept;

   private:
    enum Word : unsigned { kBlocks, kLoopMin, kLoopMax, kLoopLo, kLoopHi, kRenderMin, kRenderMax, kRenderLo, kRenderHi, kOutMin, kOutMax, kOutLo, kOutHi, kWords };
    void bucket(unsigned first, std::uint32_t v) noexcept;  // min, max, then the 64-bit sum as two words
    std::atomic<std::uint32_t> seq_{0};
    std::atomic<std::uint32_t> w_[kWords] = {};
  };

  struct Instance {
    const o3m_module* mod = nullptr;
    void* state = nullptr;
    std::atomic<std::uint8_t> ready{0};  // published by bind() (release), withdrawn by retire_all(); read by the render (acquire)
    std::uint8_t slot = 0;
    std::uint8_t track = kNone;          // render only from here on: the track whose note this is
    std::uint8_t held = 0;
    std::uint8_t held_overlap = 0;  // the held note was played over another on purpose (a live key, the audition): the only kind a later live key may slide into
    std::uint8_t active = 0;
    std::uint16_t fault_id = 0;
    std::uint16_t last_note = 0;
    // OS-MODPITCH (render only): the module's pitch call (null for a module of ABI 1.7 and below, or one that has none), the ramp the host walks, and where it stands (kRamp*).
    void (*pitch)(void* state, float semitones, float per_frame) = nullptr;
    PitchRamp ramp;
    std::uint16_t ramp_blk = 0;  // the block (blocks_, low 16 bits) of the last pitch() stretch: the block-start call does not repeat what a note or an FX did in the same block
    std::uint8_t ramp_on = 0;
    gov::InstanceGov gov;  // OS-GOV: the measured cost, the time spent in this block, the safety net's counters
    // OS-303PERF, render only: this block's cycles so far in the module's render() alone, in the conversion (to Q15) and in the zero-fill of the block; account_block() folds a full block into `perf`.
    std::uint32_t acc_render = 0;
    std::uint32_t acc_out = 0;
    std::uint32_t acc_zero = 0;
    PerfCell perf;
  };

  // OS-MODPITCH: ramp_on. Walk: the ramp is moving, and the next block start calls pitch() with the next stretch. Settle: it has landed, the next block start calls pitch(target, 0) once (the module
  // follows the last slope until told). Hold: the offset stays at the target, no calls. The first two are what the render tests (ramp_on & kRampDue).
  static constexpr std::uint8_t kRampWalk = 1, kRampSettle = 2, kRampHold = 4, kRampDue = 3;
  // What a pattern note on a track with a Slide or Glide FX is, as the note_on() flags it adds: 0 an ordinary note (no ramp), O3M_NOTE_RAMP a note that starts a ramp, with O3M_NOTE_LEGATO when it slides
  // over the note that sounds on the track, or kTie (a Slide or Glide note on the note that sounds: nothing happens).
  static constexpr unsigned kTie = 0xFFu;
  O3OS_AUDIO_COLD unsigned ramp_plan(Instance& in, unsigned track, unsigned note, unsigned prev1) noexcept;  // per note under an FX, in flash; leaves the ramp in in.ramp for ramp_start
  O3OS_AUDIO_COLD void ramp_start(Instance& in) noexcept;                  // per ramping note: the first pitch() call, for the rest of the block
  O3OS_AUDIO_COLD void ramp_block(Instance& in, std::uint32_t begin, bool first) noexcept;  // once a block per instance with a ramp: when it is walking or settling, the next stretch's pitch() call
  O3OS_AUDIO_COLD void ramp_call(Instance& in, std::uint32_t ahead) noexcept;
  O3OS_AUDIO_COLD void ramp_snap(Instance& in) noexcept;                   // the offset back to 0 with one call (the FX ended, or a reset)
  O3OS_AUDIO_COLD void pitch_call(Instance& in, float semitones, float per_frame) noexcept;

  O3OS_AUDIO_HOT void apply(const Command& c) noexcept;
  O3OS_AUDIO_COLD void audition_note(unsigned i, const Command& c) noexcept;  // OS-GOV: a `diag mod note` (a ring command) starts like any note: judged first (per note, in flash)
  O3OS_AUDIO_COLD bool admit(unsigned i, unsigned track) noexcept;  // OS-GOV, per note: may a note start on instance i, for `track`? (the governor's verdict; a refused note releases the track's note; in flash)
  O3OS_AUDIO_COLD void account_block() noexcept;  // OS-GOV, once a block if an instance rendered: the estimates, the safety net (in flash)
  O3OS_AUDIO_COLD void silence(unsigned i) noexcept;  // OS-GOV: the safety net ends the instance's note, fades out its block and stops rendering it
  O3OS_AUDIO_COLD void profile(Instance& in) noexcept;  // OS-303PERF, per block per instance, before the governor takes the block's figures: a full block goes into the instance's perf cell
  O3OS_AUDIO_COLD void start_note(unsigned i, unsigned track, unsigned note, unsigned velocity, bool overlap) noexcept;  // per note, in flash (OS-MODPITCH: it grew with the pitch FX and SRAM_L had 24 bytes of its 27 KiB left)
  O3OS_AUDIO_INLINE std::uint16_t enter(const Instance& in) noexcept { return hooks_.enter != nullptr ? hooks_.enter(in.fault_id) : std::uint16_t{0}; }
  O3OS_AUDIO_INLINE void leave(std::uint16_t previous) noexcept {
    if (hooks_.enter != nullptr) {
      hooks_.enter(previous);
    }
  }

  Instance in_[kMaxModuleInstances];
  std::atomic<std::uint8_t> slot_[kModuleSlots];
  std::atomic<std::uint8_t> gate_[kModuleSlots];  // 1: the slot's instrument is not a module (routed() is false)
  std::uint8_t owner_[kModuleTracks];   // render only: the instance whose note is on the track
  // OS-MODPITCH, render only (fx_event, start_note): the Slide FX (sixteenths of a semitone, signed) and the Glide FX (0..100) a track holds while its bit of slide_on_ / glide_on_ is set, and the last
  // module note + 1 of the track (0: none; kept across note ends and stops: the glide's memory, apart from the sampler's). A single writer, the render.
  struct TrackFx {
    std::int16_t slide;
    std::uint8_t glide;
    std::uint8_t last1;
  };
  TrackFx tfx_[kModuleTracks] = {};
  std::uint16_t slide_on_ = 0;
  std::uint16_t glide_on_ = 0;
  std::uint32_t tempo_x100_ = 12000;   // the engine's tempo (set_tempo_x100): a ramp that has started keeps its length
  std::uint8_t seg_pos_ = 0;           // the sample of the block the render has reached (begin_block: 0, render(): its end): where an event's pitch() stretch starts
  std::uint32_t touched_ = 0;           // render only: the instances whose block holds this block's audio
  bool prof_ = false;                   // render only: this block takes the perf stamps (perf_on_ as it was at the block start)
  float* scratch_ = nullptr;
  std::int16_t* block_[kMaxModuleInstances] = {};
  core::SpscRing<Command, kCommandRing> ring_;
  std::atomic<std::uint32_t> blocks_{0};  // block starts the render has seen
  std::atomic<std::uint32_t> perf_on_{0};     // OS-303PERF: the split is on (reset_perf() sets it, the main loop's one write; the render reads it once a segment)
  std::atomic<std::uint32_t> perf_reset_{0};  // OS-303PERF: reset_perf() requests (main loop writes) ...
  std::atomic<std::uint32_t> perf_seen_{0};   // ... and how many the render has served (render writes); they differ while a reset waits
  std::uint32_t retire_at_ = 0;
  bool pending_ = false;  // retire_all() withdrew instances that release_all() has not yet shut down
  Hooks hooks_;
  o3m_host host_{};
  Stats stats_;
  static std::uint32_t no_clock() noexcept { return 0u; }
  gov::Clock cycles_ = &no_clock;  // OS-GOV: set before the audio starts, read by the render (never null)
  bool governed_ = false;          // a counter was given: the instances are measured and the notes judged
  gov::Governor gov_;
};

}  // namespace o3os::audio
