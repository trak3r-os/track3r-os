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
// OS-INSFX S6, THE MASTER INSERT (section 2 of the spec; new Community OS behaviour): `where` kMasterWhere (32 = track 16, slot 0) is the master's insert, between the master chain's Bass and Limiter stages
// (MasterStage::Insert). It is DUAL MONO: TWO instances of the module, L on the left plane and R on the right, each with its own state block and the same parameters, so it takes TWO of the kInstances records
// (the cap counts instances: a master insert and three track inserts fill it). The pair is one binding with one set of commands: the main loop's `where` 32 is the left record, the right record is kMasterWhere + 1
// (an internal key, never a public `where`; parameter changes name it, the Activate / Deactivate / Bypass of the pair name only the left one, and the render applies them to both in one boundary so the two
// ramps are in step), and `Inst::mate` links the two. The stage is InsertHost::run_master, a MasterFn: float planes in place, no Q15 round trip. After the module's render() each plane is SCRUBBED: a NaN or an
// infinity (an exponent of all ones) becomes 0, so neither the limiter's delay line (which would keep it for good) nor the converter ever sees one; finite values are the limiter's to catch. A forced bypass (a
// net) takes the pair, never one channel. No tail logic: the stage runs every block while it is not bypassed, so a ring-out is heard without a strip to keep live.
//
// OS-INSFX-ABI2 A7 (docs/specs/OS-INSFX-ABI2.md 3.3; new Community OS behaviour): a STEREO effect (a shape of Capable or Widens, `render_st`) on the master is ONE instance: `render_st(l, r, l, r, n)` in place on the
// two float planes, the S6 scrub after it, one of the kInstances records (`mate` kNone, no reservation of a side block). A mono effect stays dual mono (two instances, S6).
//
// STEREO (OS-INSFX-ABI2 A4, docs/specs/OS-INSFX-ABI2.md 3.2; new Community OS behaviour). An effect of ABI 1.12 with `render_st` has a SHAPE per instance: WIDENS (O3M_FX_WIDENS: makes a stereo image from a mono
// input, runs render_st on every block) or CAPABLE (render_st only on a block whose input has S, its mono render otherwise). On such a block run() takes the track's M (io) and the S in force (Graph::side, scaled by
// the mono inserts before it in the strip: Graph::side_gain), makes L = M + S and R = M - S in the graph's two float planes, calls render_st, goes back by the module rule to M' = (L + R) / 2 into io and S' = (L - R) / 2
// into the host's ONE shared S block (`sblk_`, 64 x int16, SRAM_U), and hands it to the strip (Graph::feed_side), whose side_pass adds it to MAIN. The shared block is safe: side_pass consumes a track's S before the next
// track's inserts run, and a second stereo insert of the same track reads it as its input and overwrites it after the conversion. A transition (bind, bypass, forced bypass, clear) blends the dry pair, recomputed from
// io and the input S, with the effect's, per sample (no extra plane). The slot's stereo mark (Graph::set_insert_stereo) tells side_pass that the S it gets is the insert's own (its gain is unity again). begin_block
// (the end of the block) nulls every side pointer that still holds the shared block, so no S outlives its block (an idle strip, an unbind); Engine::feed_sides keeps writing the voices' S. A mono effect, or a
// stereo-capable one on a block without S, runs the code it always had, bit for bit. All in flash; SRAM_U +144 B measured (the block 128 B, its track mask, the Graph's mask 4 B, padding); `play_block` and `run_tracks` are untouched.
//
// THE SIDECHAIN KEY (OS-INSFX-ABI2 A6, docs/specs/OS-INSFX-ABI2.md 3.5; new Community OS behaviour). An effect with an O3M_PARAM_KEY parameter (ABI 1.12) names a track whose level it reads through
// host->track_level. The host records the key tracks of each running instance (`Inst::key`, read from the blob at the Activate and moved by every ParamChange of a KEY parameter) and begin_block folds them
// into the key mask (module::g_keys.mask, `key_mask()`). A keyed track MEASURES the peak of its mono block after its inserts, before the fader, in insert slot B: an empty slot B (or one whose module is
// bypassed) holds the host's KEY TAP (`key_tap`, an InsertFn that writes the peak and changes nothing; wet 0, so a stereo voice's S passes it), a module in slot B is installed as `run_keyed`, which runs
// the module and measures its output. The word is in module/key_state.h (the RAN flag: this block's or the previous one's); a muted key track still keys (Engine::block_end takes the key mask out of the voice gate).
// All in flash (audio/src/insert_key.cpp); the tap is not an instance (no cap, no governor; its few cycles stay in the base); SRAM_U 68 B (the store) + 2 B (`tap_mask_`) + 8 B (`Inst::key`).
//
// RT RULES for run(), begin_block(), apply_param(): no allocation, no I/O, no blocking, no printf, no virtual call, no libm, no double, bounded loops.

#include <o3module.h>

#include <o3os/audio/graph.h>
#include <o3os/audio/hot.h>
#include <o3os/audio/module_governor.h>
#include <o3os/audio/module_voices.h>
#include <o3os/audio/voices.h>
#include <o3os/core/audio_clock.h>
#include <o3os/core/param.h>
#include <o3os/core/ring.h>
#include <o3os/hal/audio.h>
#include <o3os/module/key_state.h>

#include <atomic>
#include <cstdint>

namespace o3os::audio {

class InsertHost {
 public:
  static constexpr std::uint32_t kBlock = hal::audio::kBlock;
  static constexpr unsigned kInstances = gov::kMaxInserts;
  static constexpr unsigned kMasterWhere = kTracks * 2u;    // S6: the master insert's key (track 16, slot 0): its left instance; the right one is kMasterWhere + 1
  static constexpr unsigned kWhere = kMasterWhere + 2u;     // track * 2 + slot, then the master's two instances
  static constexpr unsigned kParams = O3M_PARAMS_MAX;       // the blob copy of one instance
  static constexpr std::uint32_t kCommandRing = 32;
  static constexpr std::uint32_t kMaxCommandsPerBlock = 32;
  static constexpr unsigned kExportPasses = 16;  // boundaries prepare_export may take to settle the inserts (four instances need about 6 at most)
  static constexpr std::uint8_t kNone = 0xFF;
  // OS-INSFX S2: a tail ends after this many blocks of exact zero Q15 output while the source is silent (2 s: a delay's gap between echoes), or after 10 s of silent input (blocks of the engine's rate).
  static constexpr std::uint16_t kTailZeroBlocks = static_cast<std::uint16_t>(2.0f * core::kRefRateHz / static_cast<float>(kBlock));
  static constexpr std::uint16_t kTailMaxBlocks = static_cast<std::uint16_t>(10.0f * core::kRefRateHz / static_cast<float>(kBlock));
  static_assert(kWhere <= 64u, "the parameter id's index byte holds the key (is_insert_param accepts below 64)");
  static_assert(gov::kMaxInserts >= 2u, "the master insert is two instances");
  static_assert(kBlock == 64u, "the adapter's scratch is the graph's two 64-float planes");
  static_assert(kParams <= 32u, "set_mask and dirty hold one bit per parameter");
  using Hooks = ModuleVoices::Hooks;
  using Blobs = float[kInstances][kParams];

  enum class Phase : std::uint8_t { Off, In, On, OutBypass, Bypassed, OutLeave };
  // OS-INSFX-ABI2 A4: what an effect module does with a track's S (a module of ABI 1.11 or below, and every 1.12 effect without render_st, is Mono).
  enum class Shape : std::uint8_t { Mono, Capable, Widens };
  static Shape shape_of(const o3m_module& m) noexcept {
    return (m.kind != O3M_KIND_EFFECT || m.abi_minor < 12u || m.render_st == nullptr) ? Shape::Mono : ((m.fx_flags & O3M_FX_WIDENS) != 0u ? Shape::Widens : Shape::Capable);
  }

  struct Stats {
    std::uint32_t saturated = 0;        // samples that clipped in the Q15 conversion (a NaN, which is silence, is not one)
    std::uint32_t scrubbed = 0;         // S6: samples of the master insert's output that were NaN or infinite and became 0
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
  // OS-INSFX-ABI2 A5: `pool` (optional) is the VoicePool whose side blocks the WIDENS track inserts reserve (begin_block calls set_st_reserved); null: no reservation.
  void attach(Graph& graph, ModuleVoices& voices, Blobs* blobs, VoicePool* pool = nullptr) noexcept {
    graph_ = &graph;
    pool_ = pool;
    st_res_ = 0xFF;  // A5: an impossible count, so the next begin_block pushes the reservation to the (new) pool even when it already holds it

    voices_ = &voices;
    blobs_ = blobs;
  }
  void attach_pool(VoicePool* pool) noexcept {
    pool_ = pool;
    st_res_ = 0xFF;  // A5: same, for a pool that comes after the attach
  }

  // The cycle counter the instances are measured with (the engine's). Null: no measure, the nets stay quiet.
  void set_cycles(gov::Clock clock) noexcept { cycles_ = clock != nullptr ? clock : &no_clock; }

  // ---- main loop ------------------------------------------------------------------------------------------------------------------
  enum class Bind : std::uint8_t { Ok, NoGraph, BadWhere, WhereTaken, BadModule, NoInstance, NeedState, BadState, InitFailed, NoCpu };
  // Binds an insert of effect module `mod` at `where`. `state` is mod.state_bytes of zeroed, 16-byte aligned memory, or null to reuse the state of a free record that held the same module (NeedState: none,
  // take room and call again). S6: `where` kMasterWhere binds the master's TWO instances of a mono module: `state` is the left one's memory and `state_r` the right one's (both null: reuse two free records of the
  // module; both given: two free records; NeedState when there are two free records but not two that held the module; NoInstance with fewer than two free: the cap counts instances). A7: a stereo module (render_st) binds
  // ONE instance at kMasterWhere: `state` alone, `state_r` ignored. `state_r` is ignored for a track insert. `params` / `param_len`: the stored blob (little-endian float32 per parameter, a short blob sets the first ones, non-finite values skipped). `force`: no admission
  // (a project load). Admission (admit()): the render's other work (module voices included) + the inserts running now + this module's figure (its last measure; 0 while unmeasured: the nets judge it once it
  // runs) must fit the threshold and the instance budget.
  // The Activate is pushed now or by service(); the insert is heard from the block after.
  Bind bind(unsigned where, const o3m_module& mod, std::uint16_t fault_id, void* state, const std::uint8_t* params, std::uint32_t param_len, bool force, void* state_r = nullptr) noexcept;
  // The state blocks a bind at `where` takes when it has none to reuse: 2 for the master, 1 for a track.
  static constexpr unsigned states_for(unsigned where) noexcept { return where == kMasterWhere ? 2u : 1u; }
  // OS-INSFX-ABI2 A7: the instances a bind of `mod` at `where` takes: the master's is ONE when the module is stereo (its shape is not Mono), two (dual mono) when it is mono.
  static constexpr unsigned instances_for(unsigned where, const o3m_module& mod) noexcept {
    return where == kMasterWhere && shape_of(mod) != Shape::Mono ? 1u : states_for(where);
  }
  // Unbinds: ramps the effect out and, once the render is clear of it, shuts it down (service()). False: nothing is bound there.
  bool unbind(unsigned where) noexcept;
  enum class Toggle : std::uint8_t { Done, NotBound, NoCpu };
  // Bypass on: a ramp to dry, the effect stays bound. Off: a RESET (shutdown, init, the parameters, a new generation), admitted like a bind unless `force`.
  Toggle set_bypass(unsigned where, bool on, bool force = false) noexcept;
  // A parameter: kept in the instance's copy and sent (ParamChange{Insert}, with the binding's generation). False: not bound, or the ring is full (the copy is kept: a reset applies it). The master's (S6) goes to both
  // of its instances: ONE ParamChange with the key kMasterWhere (a Learn binding names the same key), which apply_param hands to both instances.
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

  bool bound(unsigned where) const noexcept { return where < kWhere && where != kMasterWhere + 1u && wh_[where] != kNone; }
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
    bool tail;             // S2: the effect may still ring: the strip stays live for it with a silent source
    std::uint16_t gen;
    std::uint32_t cost;    // the measured cycles of one block, 0 never measured
  };
  bool info(unsigned i, Info& out) const noexcept;
  // The last measure of the insert at `where`, cycles (0: none; the master's is both instances'). The page's figure.
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
  // S6: the Graph's MasterFn of the master insert's stage; `state` is the left record, which names the right one (`Inst::mate`). A7: a stereo module is ONE record (`mate` kNone) and takes this same stage.
  O3OS_AUDIO_COLD static void run_master(void* state, float* left, float* right, std::uint32_t n) noexcept;
  // OS-INSFX-ABI2 A6: the tracks (bit t) some running insert names as its key (the console's `ins`; Engine::block_end exempts them from the voice gate). Any context (a word).
  std::uint16_t key_mask() const noexcept { return module::g_keys.mask.load(std::memory_order_relaxed); }
  // The tracks (bit t) whose insert slot B holds the key tap now (render-written, read by the tests and the console).
  std::uint16_t taps() const noexcept { return tap_mask_; }

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
    std::uint8_t mate = kNone;      // S6: the other record of the master's pair (set before the Activate is pushed, cleared when the pair is freed); kNone for a track insert
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
    bool idle = false;              // the module's last render() returned 0 (or its tail ended: S2)
    // OS-INSFX-ABI2 A4 (render only): the module's Shape (set at the Activate) and whether the slot is marked stereo in the Graph now (run() writes the Graph only on a change).
    std::uint8_t shape = 0;
    bool st_on = false;
    // S2, the effect's tail (render only): `tail` mirrors Graph's Insert::tail so run() calls the Graph only on a change; `zero` counts the blocks in a row whose Q15 output was exactly 0 with the input silent (a
    // live source resets it: a pre-delay's zeros while playing never count) and `age` the blocks in a row whose input was exactly 0, both saturating at the limit; a tail needs render() == 1, zero < kTailZeroBlocks and age < kTailMaxBlocks.
    std::atomic<bool> tail{false};  // (relaxed: the render writes on a change, the console reads it)
    std::uint16_t zero = 0;
    std::uint16_t age = 0;
    float mix = 0.0f;               // the wet share now, and where this block takes it
    float target = 0.0f;
    std::uint32_t acc = 0;          // this block: cycles in run()
    std::uint32_t prev = 0;         // the previous block's sample (0: none since the bind)
    std::uint32_t est_at = 0;
    std::uint8_t over = 0;          // consecutive blocks over the instance budget
    std::uint8_t key[2] = {0, 0};   // OS-INSFX-ABI2 A6 (render only): the key tracks this instance names, 0 = none, else the parameter's value 1..16 (track + 1), in the order of its KEY parameters
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
  bool admit(std::uint32_t cost, bool first_bind, unsigned copies = 1u) const noexcept;
  Inst* mate_of(const Inst& in) noexcept { return in.mate != kNone ? &in_[in.mate] : nullptr; }
  void sync_pair(Inst& lead) noexcept;  // S6: the right record follows the left one's main-loop flags (the pair has one set of commands)
  bool set_param_at(unsigned where, unsigned id, float value) noexcept;
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
  // The slot of a record in the graph: a track's insert slot, or (S6) the master chain's Insert stage (one stage for the pair; a track's wet share and tail do not exist there).
  void slot_wet(const Inst& in, unsigned wet0, unsigned wet1) noexcept;
  void slot_bypass(const Inst& in, bool bypass) noexcept;
  void slot_clear(const Inst& in) noexcept;
  O3OS_AUDIO_COLD void activate(Inst& in, unsigned track, unsigned slot, std::uint16_t gen) noexcept;
  O3OS_AUDIO_COLD void run_plane(Inst& in, float* plane, std::uint32_t n) noexcept;
  void clear_tail(Inst& in) noexcept;  // S2: no tail (the slot's byte in the Graph too), counters at 0
  std::uint16_t enter(const Inst& in) noexcept;  // out of line: about 14 call sites (OS-INSFX flash cut)
  void leave(std::uint16_t previous) noexcept;
  // OS-INSFX-ABI2 A4: the adapter once the stereo decision is made (`stereo`, the input S `sin` or null, the S gain `g0`..`g1` of the mono inserts before it, 0..255); run() resolves them from the Graph, the bench gives its own.
  O3OS_AUDIO_COLD static void process(Inst& in, std::int16_t* io, std::uint32_t n, bool stereo, const std::int16_t* sin, std::uint32_t g0, std::uint32_t g1) noexcept;
  // Steps 2 to 6 of the spec's 3.2: M and S to L / R, render_st, the transition blend, back to M' and S' (the shared block, handed to the strip unless it is all zero). Returns render_st's value; `s_zero`: S' is all zero.
  O3OS_AUDIO_COLD std::uint32_t stereo_block(Inst& in, std::int16_t* io, std::uint32_t n, const std::int16_t* sin, std::uint32_t g0, std::uint32_t g1, bool ramp, bool& s_zero) noexcept;
  // OS-INSFX-ABI2 A7: the master's ONE stereo instance, in run_master: render_st(l, r, l, r, n) in place on the two float planes, the S6 scrub after it.
  O3OS_AUDIO_COLD void run_master_st(Inst& in, float* left, float* right, std::uint32_t n) noexcept;
  O3OS_AUDIO_COLD void clear_sides() noexcept;  // step 8: no side pointer holds the shared block
  bool restart_module(Inst& in) noexcept;  // shutdown, init, the stored values again: false when init() refused
  // OS-INSFX-ABI2 A6 (insert_key.cpp, flash, render context). The key tap and the keyed slot-B module's InsertFn; the key tracks of a record at its Activate and after a KEY ParamChange; the boundary's work.
  O3OS_AUDIO_COLD static void key_tap(void* state, std::int16_t* io, std::uint32_t n) noexcept;
  O3OS_AUDIO_COLD static void run_keyed(void* state, std::int16_t* io, std::uint32_t n) noexcept;
  static bool key_ok(const o3m_module& m) noexcept { return m.kind == O3M_KIND_EFFECT && m.abi_minor >= 12u && m.params != nullptr; }  // a module of 1.11 or below never has the flag honoured
  O3OS_AUDIO_COLD void read_keys(Inst& in) noexcept;
  O3OS_AUDIO_COLD void set_key(Inst& in, unsigned field, float value) noexcept;
  O3OS_AUDIO_COLD void key_boundary() noexcept;

  Graph* graph_ = nullptr;
  ModuleVoices* voices_ = nullptr;
  VoicePool* pool_ = nullptr;
  std::uint8_t st_res_ = 0;                 // A5, render: the reservation last handed to pool_. attach / attach_pool write it on the main loop, so they must run before the audio starts (no render boundary may race them)
  Blobs* blobs_ = nullptr;
  gov::Clock cycles_ = &no_clock;
  Inst in_[kInstances];
  float wet_[kBlock] = {};                     // S6: the master insert's wet block while a ramp runs (the planes are the chain's, so the dry plane stays where it is)
  std::uint8_t wh_[kWhere];                 // main loop: where -> instance
  std::uint8_t rwh_[kWhere];                // render: where -> instance (a binding the render holds)
  core::SpscRing<Command, kCommandRing> ring_;
  std::atomic<std::uint32_t> blocks_{0};    // block boundaries the render has passed
  std::uint16_t tap_mask_ = 0;              // OS-INSFX-ABI2 A6, render only: the tracks whose slot B holds the key tap now
  std::uint16_t key_prev_ = 0;              // ... and the key mask the last boundary folded (a track that left it gets its word cleared)
  std::uint32_t agg_over_ = 0;              // render: consecutive blocks the total was over the aggregate budget
  // All the render's cycles but the inserts', the module voices' included (what the aggregate net and admit() judge), as two moving means of `Governor::last_total()` less the inserts' measured cycles: a slow one (1/256 a
  // block, the base's rate of fall) and a fast one (1/32), seeded by the first block (at most the threshold; `others_n_` counts the blocks, 0: none yet, saturating at 256) and kept across an export. `others_pub_` is
  // max(slow, fast less a deadband): a sustained rise is followed in tens of blocks, a spike or two never moves it (a mean that rises faster than it falls climbs on spikes). Single writer: the render (words, relaxed);
  // admit() reads `others_pub_` on the main loop.
  std::uint32_t others_slow_ = 0;
  std::uint32_t others_fast_ = 0;
  std::uint32_t others_n_ = 0;
  std::atomic<std::uint32_t> others_pub_{0};
  std::atomic<std::uint32_t> total_pub_{0};
  bool nets_ = true;                        // render reads, the export switches it with the render parked
  // OS-INSFX-ABI2 A4, render only: the one shared S block of every stereo track insert, and the tracks a run() handed it to in this block (begin_block nulls those side pointers that still hold it).
  alignas(8) std::int16_t sblk_[kBlock] = {};
  std::uint16_t s_used_ = 0;
  Stats stats_;
};

}  // namespace o3os::audio
