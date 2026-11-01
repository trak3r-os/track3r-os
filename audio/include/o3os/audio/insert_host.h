// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-INSFX S1 (docs/specs/OS-INSFX.md, amended 2026-11-01): effect modules (.tmod kind 2) as per-track inserts. New Community OS behaviour; console only in this slice (no UI, nothing persisted).
//
// WHAT IT IS. One InsertHost owns up to kMaxInserts instances of effect modules. Each is bound to a track insert slot (INS A / INS B of the Graph, `where` = track * 2 + slot, 0..31). The Graph calls
// InsertHost::run through its InsertFn slot, in place on the track's mono Q15 block, before the sends and the fader. run() is the float / Q15 adapter: q15 -> float, the module's render() in place,
// back to Q15 by the MODULE RULE (module_q15.h: NaN is 0, saturation counted) and, in a transition block, a dry / wet ramp over the block. A module that returned 0 (silent, idle) on a silent
// input block is not called again until the input is not silent.
//
// THREADS AND THE SWAP PROTOCOL (single writer everywhere).
//   - The Graph's insert slots are written ONLY by the render context, from begin_block(), at a block boundary (Engine::block_end after the block, Engine::render_offline before it). The main loop never
//     touches a slot after the audio started.
//   - The main loop talks to the render through ONE bounded SPSC ring of 8-byte commands (Activate, Deactivate, Bypass) and through ParamChange{ParamKind::Insert} in the graph's own ring.
//   - Every binding has a 16-bit GENERATION (1..65535, never 0), new at each Activate. All commands and ParamChanges name it; the render applies a ParamChange{Insert} only when the slot is bound
//     (not leaving) AND the generation is the binding's: a change that was queued for a binding that has since left (and whose instance record was reused) is dropped and counted.
//   - A full command ring never drops an Activate, Deactivate or Bypass: the main loop keeps the intent in its own per-instance state and service() pushes again on its next tick.
//   - begin_block() runs every block whether or not the strip loop reaches the insert: a slot that is ramping out is retired (bypassed or cleared) there, never by run(): an idle strip would
//     never call run() again.
//   - A leaving instance is shut down by the main loop only after the render published retired(gen) AND two block boundaries passed (quiescent); the memory is not freed (bump arena): the record keeps
//     its module and state, the next bind of the SAME module reuses them (zero, init again).
//   - Module init() and shutdown() run on the main loop only; render() and set_param() in the render context only (set_param of a binding from the drain, as ABI says).
//   - STATE RESET: the ABI has no reset call, so a reset is shutdown() + init() on the same state block, on the main loop, through the swap protocol: un-bypass is Deactivate, wait, shutdown, zero, init,
//     parameters again, Activate (new generation). At the start of an export the render is parked and prepare_export() does the same directly.
//
// COST AND NETS (module_governor.h). InsertHost does NOT call Governor::account(): the insert cycles stay in the governor's `base`, so the instruments' note admission sees them. It measures each
// instance itself (the cycle counter around run()) for its own two nets and for the console: the estimate is the governor's (min of two blocks, moving max with decay). Per-instance net: over the
// instance budget for kMuteBlocks blocks in a row forces that instance to dry. Aggregate net: the sum of the inserts' cycles over min(instance budget, what the threshold leaves) for kMuteBlocks
// blocks forces the most expensive running insert to dry, and so on until the total fits. FORCED BYPASS is a one-block ramp to the dry signal, flagged (Info::forced), never silence; cleared by
// the user toggling bypass (a reset) or a reload. A bypassed insert is not run and not measured. Both nets are off in an export.
//
// PLACEMENT. All in flash (O3OS_AUDIO_COLD): at most kMaxInserts calls of run() a block. The Graph calls run() through a pointer; nothing in SRAM_L changes (the parameter route is the Engine's
// param hook, the boundary call is the existing block_done call site). State in SRAM_U (one object, about 0.8 KB), the parameter copies (kMaxInserts x 128 B) in memory the platform gives (SDRAM).
//
// RT RULES for run(), begin_block(), apply_param(): no allocation, no I/O, no blocking, no printf, no virtual call, no libm, no double, bounded loops.

#include <o3module.h>

#include <o3os/audio/graph.h>
#include <o3os/audio/hot.h>
#include <o3os/audio/module_governor.h>
#include <o3os/audio/module_voices.h>
#include <o3os/core/param.h>
#include <o3os/core/ring.h>
#include <o3os/hal/audio.h>

#include <atomic>
#include <cstdint>

namespace o3os::audio {

class InsertHost {
 public:
  static constexpr std::uint32_t kBlock = hal::audio::kBlock;
  static constexpr unsigned kInstances = gov::kMaxInserts;
  static constexpr unsigned kWhere = kTracks * 2u;          // track * 2 + slot
  static constexpr unsigned kParams = O3M_PARAMS_MAX;       // the blob copy of one instance
  static constexpr std::uint32_t kCommandRing = 32;
  static constexpr std::uint32_t kMaxCommandsPerBlock = 32;
  static constexpr unsigned kExportPasses = 16;  // boundaries prepare_export may take to settle the inserts (four instances need about 6 at most)
  static constexpr std::uint8_t kNone = 0xFF;
  static_assert(kWhere <= 32u, "the parameter id's index byte holds track * 2 + slot");
  static_assert(kBlock == 64u, "the adapter's scratch is the graph's two 64-float planes");
  static_assert(kParams <= 32u, "set_mask and dirty hold one bit per parameter");
  using Hooks = ModuleVoices::Hooks;
  using Blobs = float[kInstances][kParams];

  enum class Phase : std::uint8_t { Off, In, On, OutBypass, Bypassed, OutLeave };

  struct Stats {
    std::uint32_t saturated = 0;        // samples that clipped in the Q15 conversion (a NaN, which is silence, is not one)
    std::uint32_t params = 0;           // ParamChanges applied to a module
    std::uint32_t stale = 0;            // ParamChanges dropped: no binding there, it was leaving, or another generation
    std::uint32_t activations = 0;
    std::uint32_t retired = 0;
    std::uint32_t forced_instance = 0;  // forced bypasses by the per-instance net
    std::uint32_t forced_aggregate = 0; // ... by the aggregate net
    std::uint32_t ring_full = 0;        // main loop: pushes that found the command ring full and were kept for the next tick
    std::uint32_t export_unsettled = 0; // prepare_export gave up: the inserts did not settle in kExportPasses boundaries, or an effect's init failed on the restart (the export is refused)
  };

  InsertHost() noexcept;
  InsertHost(const InsertHost&) = delete;
  InsertHost& operator=(const InsertHost&) = delete;

  // ---- main loop, before the audio starts -----------------------------------------------------------------------------------------
  // `voices` gives the service table, the platform hooks and the governor; `blobs` is kInstances x kParams floats of memory the host keeps the parameter values in (the re-apply after a reset).
  void attach(Graph& graph, ModuleVoices& voices, Blobs* blobs) noexcept {
    graph_ = &graph;

    voices_ = &voices;
    blobs_ = blobs;
  }
  // The cycle counter the instances are measured with (the engine's). Null: no measure, the nets stay quiet.
  void set_cycles(gov::Clock clock) noexcept { cycles_ = clock != nullptr ? clock : &no_clock; }

  // ---- main loop ------------------------------------------------------------------------------------------------------------------
  enum class Bind : std::uint8_t { Ok, NoGraph, BadWhere, WhereTaken, BadModule, NoInstance, NeedState, BadState, InitFailed, NoCpu };
  // Binds an insert of effect module `mod` at `where`. `state` is mod.state_bytes of zeroed, 16-byte aligned memory, or null to reuse the state of a free record that held the same module (NeedState: none,
  // take room and call again). `params` / `param_len`: the stored blob (little-endian float32 per parameter, a short blob sets the first ones, non-finite values skipped). `force`: no admission
  // (a project load). Admission: the graph's base (which holds the inserts running now) + the voices that sound + this module's figure (its last measure, or kColdInsertCycles) must fit the threshold.
  // The Activate is pushed now or by service(); the insert is heard from the block after.
  Bind bind(unsigned where, const o3m_module& mod, std::uint16_t fault_id, void* state, const std::uint8_t* params, std::uint32_t param_len, bool force) noexcept;
  // Unbinds: ramps the effect out and, once the render is clear of it, shuts it down (service()). False: nothing is bound there.
  bool unbind(unsigned where) noexcept;
  enum class Toggle : std::uint8_t { Done, NotBound, NoCpu };
  // Bypass on: a ramp to dry, the effect stays bound. Off: a RESET (shutdown, init, the parameters, a new generation), admitted like a bind unless `force`.
  Toggle set_bypass(unsigned where, bool on, bool force = false) noexcept;
  // A parameter: kept in the instance's copy and sent (ParamChange{Insert}, with the binding's generation). False: not bound, or the ring is full (the copy is kept: a reset applies it).
  bool set_param(unsigned where, unsigned id, float value) noexcept;
  // One main-loop tick: pushes what a full ring held back, and finishes the leaving instances (shutdown, then free or a re-activate).
  void service() noexcept;
  // Project load: every instance leaves (service() finishes them). settled() is true when none is bound or waiting for the render; release_all() then (or after a timeout) shuts down what is left
  // and forgets every module and state (the arena they came from is about to be reset). The caller that has no render calls release_all() without waiting.
  void retire_all() noexcept;
  bool settled() const noexcept;
  void release_all() noexcept;
  // The export is about to start (OS-WAVEXP; the render is parked): applies the queued commands and finishes every transition, resets each running effect (shutdown, zero, init, parameters) so two
  // exports of a song give the same bits, and turns both nets off. export_end() turns them on again.
  // False: the transitions did not finish in kExportPasses boundaries; or an effect's init failed on the restart (that insert leaves); the nets are on again and the caller must not start the export.
  bool prepare_export() noexcept;
  void export_end() noexcept;

  bool bound(unsigned where) const noexcept { return where < kWhere && wh_[where] != kNone; }
  // For `ins` and the tests (main loop; the render writes a word at a time).
  const Stats& stats() const noexcept { return stats_; }
  struct Info {
    unsigned where;
    const o3m_module* mod;
    const void* state;
    Phase phase;           // what the render did with it (a word the render publishes)
    bool user_bypass;      // the main loop asked for bypass
    bool forced;           // a net forced it to dry (OVER BUDGET)
    bool leaving;
    std::uint16_t gen;
    std::uint32_t cost;    // the measured cycles of one block, 0 never measured
  };
  bool info(unsigned i, Info& out) const noexcept;
  // The last measure of the instance at `where`, cycles (0: none). The page's figure.
  std::uint32_t cost(unsigned where) const noexcept;
  // The total the running inserts took in the latest block that ran any, cycles.
  std::uint32_t total_cycles() const noexcept { return total_pub_.load(std::memory_order_relaxed); }
  // Blocks the render passed (begin_block calls).
  std::uint32_t blocks() const noexcept { return blocks_.load(std::memory_order_acquire); }

  // ---- render context -------------------------------------------------------------------------------------------------------------
  // A block boundary (the end of a block, or the start of an export's block): finishes the transitions of the block that ran, accounts it (the nets), applies the commands. In flash.
  O3OS_AUDIO_COLD void begin_block() noexcept;
  // The Engine's param hook for ParamKind::Insert (graph.cpp's default route). True when the module took it.
  O3OS_AUDIO_COLD bool apply_param(const core::ParamChange& c) noexcept;
  // The Graph's InsertFn of every slot this host installs; `state` is the instance record.
  O3OS_AUDIO_COLD static void run(void* state, std::int16_t* io, std::uint32_t n) noexcept;

 private:
  friend struct InsertHostTest;  // tests/audio_insert_test.cpp: a full command ring cannot be reached with four instances through the public calls
  friend struct InsertHostBench;  // platform/mk66/src/bench.cpp (`diag bench insert`, bench images only): run() on a private record, the graph's slots untouched
  enum class Cmd : std::uint8_t { Activate, Deactivate, Bypass };
  struct Command {
    Cmd kind;
    std::uint8_t inst;
    std::uint8_t track;
    std::uint8_t slot;
    std::uint16_t gen;
    std::uint8_t on;
    std::uint8_t pad;
  };
  static_assert(sizeof(Command) == 8, "a command is two words");

  // Main loop's view of an instance.
  enum class Ml : std::uint8_t { Free, Bound, Leaving };

  struct Inst {
    // main loop writes before the Activate is pushed (release), the render reads after the pop (acquire)
    const o3m_module* mod = nullptr;
    void* state = nullptr;
    std::uint16_t fault_id = 0;
    // main loop only
    Ml ml = Ml::Free;
    std::uint8_t where = kNone;
    std::uint16_t gen = 0;          // the binding's generation (0: none yet)
    std::atomic<std::uint16_t> cur_gen{0};  // gen as the render reads it (start_binding writes it before the Activate is pushed): apply() drops a command of another generation
    std::uint32_t set_mask = 0;     // parameters set since the bind: the copy in blobs_ is valid for these
    std::uint32_t dirty = 0;        // set_mask bits whose value has not been sent to the render yet (a ParamChange is sent only once the render holds the binding)
    bool act_pushed = false;
    bool deact_pushed = false;
    bool byp_want = false;          // the main loop asked for bypass
    bool byp_pushed = false;
    bool then_reactivate = false;   // Leaving ends in a new binding of the same module (a reset)
    bool shut = true;               // shutdown() ran (or init() never did): the state is not live
    // render only
    InsertHost* host = nullptr;
    Phase phase = Phase::Off;
    std::uint16_t rgen = 0;         // the generation the render holds
    std::uint8_t track = 0;
    std::uint8_t slot = 0;
    bool idle = false;              // the module's last render() returned 0
    float mix = 0.0f;               // the wet share now, and where this block takes it
    float target = 0.0f;
    std::uint32_t acc = 0;          // this block: cycles in run()
    std::uint32_t prev = 0;         // the previous block's sample (0: none since the bind)
    std::uint32_t est_at = 0;
    std::uint8_t over = 0;          // consecutive blocks over the instance budget
    // the render writes (words, relaxed), any context reads
    std::atomic<std::uint8_t> pub_phase{0};
    std::atomic<std::uint8_t> forced{0};
    std::atomic<std::uint32_t> est{0};       // the cost estimate; kept when the same module is bound again
    std::atomic<std::uint16_t> pub_gen{0};   // the generation the render holds (0: none): the main loop sends ParamChanges only for a binding that is live
    std::atomic<std::uint16_t> retired_gen{0};
    std::atomic<std::uint32_t> retired_blk{0};
  };

  static std::uint32_t no_clock() noexcept { return 0u; }
  // main loop
  bool push(const Command& c) noexcept;
  bool retired_ok(const Inst& in) const noexcept;
  void start_binding(Inst& in, unsigned where, const std::uint8_t* params, std::uint32_t param_len) noexcept;
  bool admit(std::uint32_t cost) const noexcept;
  bool init_module(Inst& in) noexcept;
  void shutdown_module(Inst& in) noexcept;
  void apply_blob(Inst& in) noexcept;
  void clear_where(Inst& in) noexcept;
  bool live(const Inst& in) const noexcept;
  void flush_params(Inst& in) noexcept;
  void finish_leave(Inst& in) noexcept;
  std::uint16_t next_gen(std::uint16_t g) const noexcept { return static_cast<std::uint16_t>(g == 0xFFFFu ? 1u : g + 1u); }
  // render
  O3OS_AUDIO_COLD void apply(const Command& c) noexcept;
  O3OS_AUDIO_COLD void account() noexcept;
  O3OS_AUDIO_COLD void force_bypass(Inst& in) noexcept;
  void leave_ramp(Inst& in, Phase to) noexcept;
  std::uint16_t enter(const Inst& in) noexcept;  // out of line: about 14 call sites (OS-INSFX flash cut)
  void leave(std::uint16_t previous) noexcept;
  bool restart_module(Inst& in) noexcept;  // shutdown, init, the stored values again: false when init() refused

  Graph* graph_ = nullptr;
  ModuleVoices* voices_ = nullptr;
  Blobs* blobs_ = nullptr;
  gov::Clock cycles_ = &no_clock;
  Inst in_[kInstances];
  std::uint8_t wh_[kWhere];                 // main loop: where -> instance
  std::uint8_t rwh_[kWhere];                // render: where -> instance (a binding the render holds)
  core::SpscRing<Command, kCommandRing> ring_;
  std::atomic<std::uint32_t> blocks_{0};    // block boundaries the render has passed
  std::uint32_t agg_over_ = 0;              // render: consecutive blocks the total was over the aggregate budget
  // Cycles of the inserts that stopped running (forced, bypassed, unbound) which the governor's base still holds: base falls by 1/256 a block only, so the nets and admit() take this off it; it falls at the
  // same rate. Single writer: the render (words, relaxed); admit() reads it on the main loop.
  std::atomic<std::uint32_t> gone_{0};
  std::atomic<std::uint32_t> total_pub_{0};
  bool nets_ = true;                        // render reads, the export switches it with the render parked
  Stats stats_;
};

}  // namespace o3os::audio
