// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The fixed audio graph (OS-005b; ADR 0013 D2, D4, D6, docs/specs/OS-005-audio-engine.md "005b"): a pure, block-based renderer. One call,
// Graph::render(), turns the RX block and the pending parameter changes into the TX block, callable from the MK66's render interrupt and,
// later, from an offline-render job in pull mode (it touches nothing but its own state, its TapMem and the two blocks it is handed). No virtual
// call, no allocation, no libm and no flash call on the per-block path; the topology is a fixed table built at construction.
//
//   RX block -> input strip  (level, routing bits, sends)                                      ---+
//   track source (voices later; today a test source or a caller-fed block), Q15 mono               |
//     -> insert 1 -> insert 2 (reserved, bypassed) -> sends A/B (PRE-fader) -> level ramp ->      |
//        post-fader mono block = meter + StemTap -> pan law -> MAIN | CUE | REC                   |
//   SNDA bus -> effect slot A (reserved) -> return -> MAIN (+ CUE)  [ = delay ]                   +-> int32 accumulators (Q19), rounded and
//   SNDB bus -> effect slot B (reserved) -> return -> MAIN (+ CUE)  [ = reverb ]                  |   saturated ONCE per bus (D2)
//   MAIN -> master chain (kMasterChain: headroom, saturation, EQ, bass, limiter, space, bit depth; every stage a reserved slot, bypassed
//           until its behaviour is specified; with a stage active the bus goes to float STRAIGHT from the accumulators) -> master level   |
//   CUE  -> cue level                                                                             |
//   OUT MODE: stereo MAIN, or MAIN (mono) on the left jack and CUE (mono) on the right  -> the TX block
//   taps: meters (per track, bus, master), scope ring, spectrum ring, StemTap, REC bus -> the sample arena (taps.h, meters.h)
//
// Formats (D2): int16 Q15 planes, int32 accumulators at Q19 (a Q15 sample is 16 units: the product keeps four more fractional bits, so a
// source's error is 1/16 LSB and a sum of tracks has no DC offset), Q16 gains in [0, kMaxGain] applied at Q20; float only inside a master-chain
// stage. THE ONLY SATURATION OF A BUS is its output: the rounding step from Q19 to int16 (sat_clear) for MAIN without an active chain, CUE, REC and
// the sends, and float_to_q15 for MAIN after the chain: a sum that exceeds full scale reaches the chain's float stages unclipped (its headroom
// stage, PAR-110, can use it) and is clipped once, at the end. The kernels are kernels.h.
//
// The strip pass. Tracks are made and used ONE AT A TIME: a track's block is generated (or copied from its feed) into one of two shared scratch
// blocks, goes through its inserts, sends and fader, and is added to the buses; nothing of it is kept but what a tap needs (its meter, a staged
// copy for the stems). Two tracks that open MAIN are processed together (kernel::gain_pan2 / gain_mac2: one load and store of the accumulators for
// both), and a track panned at the centre (the pan law's two gains equal) adds into one centre accumulator instead of two (kernel::gain_mac): the
// bus adds it to both sides when it is saturated. The integer sums commute, so none of this changes a bit of the result.
//
// Contexts. render() and everything it calls run in ONE context, the render line (hal/audio.h). The main loop's only way in is the ParamChange
// ring (params()), the feed pointers and the atomics of the taps; configure() and the install_* calls are for before start (or with the render
// stopped). Everything that is a "render context only" accessor says so.
//
// Gains that carry no signal do not ramp. A strip that is silent this block (no source, muted by the underrun, a shut fader) does not run its
// smoothers; they are snapped to their targets instead (Smoother::snap), so that when the signal comes back the gains are where they were
// asked to be, not wherever a ramp was interrupted: a mute, a route closed under the mute and the unmute must not leak into the closed route.
//
// Solo. The mixer's SOLO is implemented as a mute of every other track (the input strip is not muted by it). A muted track's amplifier is
// muted and, when the track is flagged muted, so are its delay and reverb sends. By default a solo of another track also silences a track's
// sends: solo_mutes_sends is true. set_solo_mutes_sends(false) is the other reading: a solo mutes the other tracks' amplifiers only and their
// sends stay open (the half OS-005g's export is to use). Which of the two a live solo of the official firmware does is an open research question
// (Q). The default is a Community OS choice, one constant,
// tested both ways.
//
// Send effects (OS-SENDFX: SNDA is the delay of send_delay.h, SNDB the reverb of send_reverb.h; the platform installs them): an effect slot that has a processor is run every block, bypassed or not. BYPASS closes its INPUT: the
// effect is handed silence, so what it has already taken in rings out through its return (a delay's repeats, a reverb's tail); bypass is not a cut. A
// slot with no processor has no return at all. This takes effect when real effects arrive; the slots of 005b only have the test doubles.
//
// The master limiter (OS-LIMIT, audio/master_limiter.h; the platform installs it with install_master_limiter): the chain's limiter stage, ALWAYS ON (a base limiter that holds MAIN to -0.3 dBFS before the
// int16 conversion, 63 samples behind: MAIN is that late against CUE and REC, which are not in the chain). The master LIMITER switch, the project's LIMITER MODE and the three controls reach it as
// ParamChanges (graph_params.h). The MAIN meters (meter_ch::kMainL / kMainR) then measure the sum as it ARRIVES at the limiter (clamped to int16 for the meter), so their clip count is the overload the
// limiter had to catch; the master meters measure what is heard. Without a limiter installed the chain is as it was: MAIN is metered after it, with no stage active the bus goes through untouched.
//
// Monitoring (radar card `monitoring-while-recording`, orchestrator decisions). The codec RX is the INPUT STRIP: a stereo source row
// with a level and routing bits (and sends) that goes through the engine into MAIN and CUE, so it can be recorded (REC), sent to the effects
// and heard with the playback, which never stops for the recorder (the render does not look at the recorder's state). It starts at level 0:
// a monitor that is audible by default would feed back the moment OUT is patched to IN (the HW-A latency loop) or a microphone meets a
// speaker; the app raises it, as a deliberate act (`diag mon` on the console). Its latency through the engine is RX slot + render + TX slot,
// no more inside the graph: sample n of the RX block is sample n of the TX block it goes into.
//
// USB return (OS-006e; ADR 0012 Decision 4 and amendment 6). The host's stereo stream (usb::ReturnReader) is NOT part of the input strip: it has a route of its own, to the OUTPUTS, at its own
// level (the ParamChange master::UsbReturnLevel, the mixer level law, 100 = unity = the default; times the master level). So a DAW that plays into the Tracker is heard on the line out and the
// headphones at the level it sends, with the monitor at 0, and the line-in's level, routing bits and sends are the line-in's alone. Neither the master DRY level nor DRY MUTE touch it (they are
// the song's direct mix), and the input strip is outside the DRY LEVEL too: that fader is 86 in every v7+ project, which would have lowered a live input by 5.6 dB.
// NO LOOPBACK BY DEFAULT: the return is added to the heard planes at the very end (run_levels), after MAIN's chain and master level, and is in none of the captures: not MAIN (so not the MAIN
// meters, the stems' Master L / R or RecTap::Source::Master, which resamples MAIN) and not CUE or REC. A DAW that monitors the Tracker's USB capture while it plays into the Tracker would
// otherwise build a feedback loop, and a backing track would be recorded back. It bypasses the limiter (the heard sum saturates at full scale); the master meters, the scope and the spectrum
// see it, as they see what is heard. CUE and REC are not routes of the return either (TODO: a routing-matrix row; a loopback setting would be device row 17). While the recorder's source is the USB
// (InputFeed::ReturnOnly) the strip carries the stream instead and this route ramps shut over a block (and open again after it). A disabled return (no host streaming) costs one atomic load a block.
//
// Real-time rules: no heap, no blocking, bounded loops (kBlock, the strip count), no double.

#include <o3os/audio/graph_params.h>
#include <o3os/audio/hot.h>
#include <o3os/audio/kernels.h>
#include <o3os/audio/loop_player.h>
#include <o3os/audio/master_dj_filter.h>
#include <o3os/audio/master_kill_eq.h>
#include <o3os/audio/master_scratch.h>
#include <o3os/audio/master_limiter.h>
#include <o3os/audio/meters.h>
#include <o3os/audio/metro.h>
#include <o3os/audio/smoothing.h>
#include <o3os/audio/app_tap.h>
#include <o3os/audio/tap_mem.h>
#include <o3os/audio/taps.h>
#include <o3os/audio/test_signal.h>
#include <o3os/core/param.h>
#include <o3os/core/ring.h>
#include <o3os/hal/audio.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace o3os::audio {

// The platform's free-running cycle counter (DWT CYCCNT on the MK66): differences only, unsigned subtraction. The pure library takes it as a
// function pointer because it may not include a platform; the call is a dozen times a block.
using CycleFn = std::uint32_t (*)() noexcept;

// The ParamChange of ADR 0009 rev 1 (OS-002d): `{ParamId, value, ramp}`, one type shared with the UI side (core/param.h), whose producer is the
// main loop (edits, MIDI Learn) and whose consumer is this engine, in the graph's pass (after the block's voices). The ring (256 entries, ADR 0013 D6) and the per-block bound on
// the drain are core's constants too, named here for the engine's users.
using ParamChange = core::ParamChange;
using core::kMaxParamsPerBlock;
using core::kParamRingSize;
using ParamRing = core::ParamRing;

// The master chain (ADR 0013 D4, order B: saturation first because 1.7.0's PAR-109 says so; space unsourced). Marked for a compatibility/hardware check: the order of the
// official firmware's chain is an open research question (Q); a measurement on an official unit, if ever done, may reorder it, and the order is this one table (the slot
// index is the position in it). `par` is the parity row (PAR-xxx number) each stage comes from (0: none).
// OS-DJ (orchestrator, 2026-10-19): the EQ and bass slots, which hold the DJ page's kill EQ and DJ filter, sit AHEAD of the limiter, so the limiter catches the peaks they add (an
// all-pass split and a filter that rings near its open end both raise the peaks of a limited mix; after the limiter the converter would clip them). Neutral at rest, as before.
enum class MasterStage : std::uint8_t { Headroom, Saturation, Eq, Bass, Limiter, Space, BitDepth, Count };
struct MasterStageInfo {
  MasterStage id;
  const char* name;
  std::uint16_t par;
};
inline constexpr MasterStageInfo kMasterChain[] = {
    {MasterStage::Headroom, "headroom", 110},     {MasterStage::Saturation, "saturation", 109}, {MasterStage::Eq, "eq", 55},
    {MasterStage::Bass, "bass", 56},              {MasterStage::Limiter, "limiter", 58},        {MasterStage::Space, "space", 57},
    {MasterStage::BitDepth, "bit depth", 0},
};
inline constexpr unsigned kMasterStages = sizeof(kMasterChain) / sizeof(kMasterChain[0]);
static_assert(kMasterStages == static_cast<unsigned>(MasterStage::Count), "one table row per stage");

// The processors the slots take (their contracts are later slices'; the signatures are what a module or a core effect will have to meet).
//   InsertFn   in place on the track's mono Q15 block, before the sends and the fader
//   SendFxFn   the send bus (mono Q15) in, the stereo return planes out
//   MasterFn   in place on the MAIN pair as float planes of `n` frames (+-1.0 = full scale; the planes may exceed it), in chain order
using InsertFn = void (*)(void* state, std::int16_t* io, std::uint32_t n) noexcept;
using SendFxFn = void (*)(void* state, const std::int16_t* in, std::int16_t* out_l, std::int16_t* out_r, std::uint32_t n) noexcept;
using MasterFn = void (*)(void* state, float* left, float* right, std::uint32_t n) noexcept;
// OS-SENDFX: what a send effect takes of the ParamChanges that name it (sendfx:: in graph_params.h): its own field number and the value; false when it is not one of its fields or the value is out
// of range (the graph then counts the change rejected). Called from the ParamChange drain (the graph's pass, after the block's voices), in flash, once per change.
using SendFxParamFn = bool (*)(void* state, unsigned field, float value) noexcept;

// The tracks the graph has: the model's 16 slots (model/limits.h kMaxTracks, held equal by tests/audio_model_ids_test.cpp), 12 of them
// active in a new project (kDefaultActiveTracks there), as a bit mask for configure().
inline constexpr unsigned kTracks = 16;
inline constexpr std::uint32_t kDefaultActiveMask = 0x0FFFu;
// The spare strip: the last track, outside the twelve of a new project. `diag mod note` plays a module on it (platform/mk66 app_main.cpp module_audition) and Engine::attach_modules switches
// it on (OS-MODSILENT: until then the strip was skipped, whatever its parameters, and the audition could not be heard nor metered).
inline constexpr unsigned kAuditionTrack = kTracks - 1u;

// The frequency of a strip's test sine: the HW-A script's 1 kHz and 1.5 kHz for the first two (the left and right channel of the tone), then
// distinct frequencies so a test can tell the strips apart.
constexpr std::uint32_t test_source_hz(unsigned track) noexcept {
  return track == 0u ? kToneHzLeft : (track == 1u ? kToneHzRight : 500u + 250u * track);
}

class Graph {
 public:
  static constexpr unsigned kStrips = kTracks + 1u;  // + the input strip
  static constexpr std::uint32_t kBlock = hal::audio::kBlock;
  static_assert(kInputStrip == kTracks, "the input strip follows the tracks");
  static_assert(kStrips <= 32u, "a strip is a bit of a 32-bit dirty mask");

  // `mem` is the taps' memory (tap_mem.h), placed by the caller; the graph resets it.
  Graph(CycleFn cycles, TapMem& mem) noexcept;
  Graph(const Graph&) = delete;
  Graph& operator=(const Graph&) = delete;

  // ---- Before start (or with the render stopped) --------------------------------------------------------------------------
  // The tracks the project has, as a mask: bit t set = track t sounds. The app builds it from the model (the first `active tracks` slots that are not
  // MIDI tracks: a MIDI track makes no audio). A strip outside the mask is skipped whatever its parameters. Default kDefaultActiveMask.
  void configure(std::uint32_t active_mask) noexcept;
  // One more strip on top of the mask in force (before start, or with the render stopped): it sounds from the next block and nothing else about the graph changes. A track outside the graph is ignored.
  void activate(unsigned track) noexcept;
  // The processors of the reserved points. Installing leaves the slot BYPASSED (the default of every slot); un-bypass it separately.
  void install_insert(unsigned track, unsigned slot, InsertFn fn, void* state) noexcept;
  void set_insert_bypass(unsigned track, unsigned slot, bool bypass) noexcept;
  // OS-INSFX S1, render context (InsertHost::begin_block, a block start: the slots are written by the render only once the audio runs). Clears the slot (fn null, bypassed).
  void clear_insert(unsigned track, unsigned slot) noexcept { install_insert(track, slot, nullptr, nullptr); }
  // The wet share (0..255) the insert has at the start and at the end of the coming block: the stereo S of a voice on the track follows 255 - wet through side_pass (one block ramp, no step when an insert
  // comes in, goes out or is bypassed). Flash.
  void set_insert_wet(unsigned track, unsigned slot, unsigned wet0, unsigned wet1) noexcept;
  // OS-INSFX S1: the two 64-float blocks of the master chain's planes, free while the strips run (the master chain fills them after the buses): an insert's float block and dry copy. Render context, in run_tracks.
  float* insert_scratch(unsigned plane) noexcept { return fl_[plane & 1u]; }
  // 0: SNDA (the delay), 1: SNDB (the reverb). `param` (OS-SENDFX) takes the slot's ParamChanges (Master kind, fields sendfx::kFieldBase[which] + n); without one they are refused.
  void install_send_fx(unsigned which, SendFxFn fn, void* state, SendFxParamFn param = nullptr) noexcept;
  void set_send_fx_bypass(unsigned which, bool bypass) noexcept;            // closes the effect's input; see "Send effects" above
  void install_master_stage(MasterStage id, MasterFn fn, void* state) noexcept;
  void set_master_bypass(MasterStage id, bool bypass) noexcept;
  // OS-LIMIT: puts `limiter` into the chain's limiter slot, un-bypassed (the base limiter is always on), resets it, and routes the limiter's ParamChanges to it (the master LIMITER switch, the
  // project's LIMITER MODE, graph_params.h master::LimiterThreshold / Attack / Release). Before start. Without one those changes are refused (the switch is only stored).
  void install_master_limiter(MasterLimiter& limiter) noexcept;
  // Render context (the Engine, where a note starts): the instrument pan 0..100 of the note that began on `track`, 50 the centre. It does not replace the strip's pan
  // (the mixer's, a MED import's), it moves it: the pan law gets strip pan + (instrument pan - 50), clamped to 0..100, so with the strip at the centre (unless the
  // mixer or a MED import moved it) the instrument's pan is the track's, and an instrument at 50 leaves the strip's pan.
  // Instant (the gain is the target from the next block's first sample): it is set at note on (whether the official firmware does likewise is an open research question), and the voice's block of a track that has no note is silent.
  // `ramp` as ParamChange::ramp_samples: 1 at a note's start; a live edit of the pan of a playing instrument (OS-LIVEP, Engine::instrument_hook) asks for 0, one block of smoothing.
  O3OS_AUDIO_HOT void set_voice_pan(unsigned track, unsigned instrument_pan, std::uint32_t ramp = 1) noexcept;
  // OS-COREFX CF2 (audio/tracker_fx.h): the tracker FX's pan is ABSOLUTE: set_track_pan makes the pan the strip plays `pan` 0..100 (50 the centre) by setting the instrument offset to pan minus the
  // strip's own pan (a MOD / S3M import hard-pans its channels with the strip, and a tracker pan is a position, not a shift); track_pan is the pan the strip plays now (strip + offset, held
  // to 0..100). `ramp` as set_voice_pan's (0: smoothed over the block). Render context, in flash, once a tick for each track a pan effect moves.
  O3OS_AUDIO_COLD void set_track_pan(unsigned track, float pan, std::uint32_t ramp = 0) noexcept;
  O3OS_AUDIO_COLD float track_pan(unsigned track) const noexcept;
  // OS-COREFX CF3 (audio/tracker_fx.h), render context, in flash, once a tick for each track a volume or pan effect moves (a changed value only; ramp 0: smoothed over the block). set_track_gain is the
  // tracker's channel volume times its global volume, 0..1: a multiplier on the strip's fader (the dry level) and on the sends, pre- or post-fader (it is the note's gain, like the volume column; a pre-fader send skips only the fader), applied where the fader is, per block,
  // in the strip's own level smoothers: no new work per sample. set_track_pan_mod is the panbrello: an offset in pan units (-100..100) added to whatever pan is in force (the strip's, the instrument's, a held
  // tracker pan), held to 0..100 by the law.
  O3OS_AUDIO_COLD void set_track_gain(unsigned track, float gain, std::uint32_t ramp = 0) noexcept;
  O3OS_AUDIO_COLD void set_track_pan_mod(unsigned track, float offset, std::uint32_t ramp = 0) noexcept;
  // OS-PERFFX P0, P2 (docs/specs/OS-PERFFX.md; new Community OS behaviour): the Perform layer's VOLUME and PANNING values of a track, each as the layer sent it (-100..100, 0: the layer holds none), in
  // fields of the strip's own (Strip::perf_gain, perf_pan) that the tracker FX leave alone: FxReset (TrackerFx::end_volumes) resets trk_gain and pan_mod, never these. recompute_strip composes them:
  // VOLUME below 0 only cuts, gain = (1 + v/100)^2 (-100 is silence, 0 and above is 0 dB: a strip's peaks are saturated to int16 before MAIN, so there is no boost), multiplied into the tracker's
  // channel gain (the dry level and the sends, as that one); PANNING adds v to the pan in force (strip + instrument + panbrello, in pan units), held to 0..100 by the law. set_perf_gain / set_perf_pan
  // (render context, in flash, a changed value only; ramp as set_track_gain's: 0 smooths over the block) mark the strip dirty. The export puts the values away and brings them back
  // (Engine::offline_enter / offline_leave), main loop with the render parked, in flash; perf_clear and perf_restore mark the strips they change dirty with ramp 1, so the export's first block has the
  // gains it would have with Perform off and the live render's first block has the layer back. Tracks only (the input strip has none).
  O3OS_AUDIO_COLD void set_perf_gain(unsigned track, int value, std::uint32_t ramp = 0) noexcept;
  O3OS_AUDIO_COLD void set_perf_pan(unsigned track, int value, std::uint32_t ramp = 0) noexcept;
  // OS-PERFFX P3 (new Community OS behaviour): the Perform layer's DELAY and REVERB, and the step FX 30 and 32, on the track's sends (which 0 the delay's, 1 the reverb's). The layer's value is an OFFSET of a
  // full send, -100..100 (-100 closes the send, +100 opens it whole, whatever the instrument's), added where the strip composes its sends (recompute_strip, after the strip's own send and the instrument's have
  // been held to 100): so every set_voice_sends caller, a note's start and a live edit of the instrument's send alike, has it, with no caller of its own to remember; like the instrument's send it is not
  // gated by the routing bit. The step FX holds the track's send for the notes while the sequencer holds it (0..100, in place of the instrument's; -1: none), and the performance layer is added to it.
  // Render context, in flash, a changed value only; ramp as set_perf_gain's. FxReset (TrackerFx::reset) and a project load (master::VoiceSendsReset) end the step FX's, never the layer's; the export
  // puts the layer away with the rest (PerfSave); it starts and ends with no step FX held (offline_quiet: clear_step_sends with ramp 1, as perf_clear's marks). The step FX are not in PerfSave on purpose: an export is only requested
  // with the live transport stopped (seq::OfflineRender::request refuses "STOP THE TRANSPORT FIRST", and the export is modal), so the sequencer holds no step FX of its own then, and what a Stop left (the Cutoff FX's precedent) is
  // ended by the next run's FxReset anyway: an export forgets it a little earlier.
  O3OS_AUDIO_COLD void set_perf_send(unsigned track, unsigned which, int value, std::uint32_t ramp = 0) noexcept;
  O3OS_AUDIO_COLD void set_step_send(unsigned track, unsigned which, int value, std::uint32_t ramp = 0) noexcept;
  O3OS_AUDIO_COLD void clear_step_sends(std::uint32_t ramp = 0) noexcept;
  struct PerfSave {
    std::int8_t gain[kTracks];
    std::int8_t pan[kTracks];
    std::int8_t send[2][kTracks];
    std::uint8_t xf[kTracks];  // OS-DJ A1: the DJ crossfader's cut (the export is clean of it too)
    float dj_filter;           // OS-DJ A2: the master DJ filter's target (the export is clean of it too)
    std::uint8_t kill_eq;      // OS-DJ A3: the master kill EQ's mask (the export is clean of it too)
  };
  O3OS_AUDIO_COLD void perf_save(PerfSave& out) const noexcept;
  O3OS_AUDIO_COLD void perf_clear() noexcept;
  O3OS_AUDIO_COLD void perf_restore(const PerfSave& in) noexcept;
  int perf_gain(unsigned track) const noexcept { return track < kTracks ? strips_[track].perf_gain : 0; }  // (for tests)
  int perf_pan(unsigned track) const noexcept { return track < kTracks ? strips_[track].perf_pan : 0; }
  int xf_gain(unsigned track) const noexcept { return track < kTracks ? strips_[track].xf_gain : 255; }  // (for tests)
  float dj_filter() const noexcept { return dj_filter_.target(); }  // OS-DJ A2: the master DJ filter's target (for tests); its stage is master_stage_active(MasterStage::Bass)
  float dj_filter_position() const noexcept { return dj_filter_.position(); }  // (for tests)
  MasterScratch& scratch() noexcept { return scratch_stage_; }  // OS-DJ C1: the app stages the ring and reads busy(); the platform sets the counter
  unsigned kill_eq() const noexcept { return kill_eq_.mask(); }  // OS-DJ A3: the master kill EQ's mask (for tests); its stage is master_stage_active(MasterStage::Eq)
  float kill_eq_gain(unsigned band) const noexcept { return kill_eq_.gain(band); }  // (for tests)
  int perf_send(unsigned track, unsigned which) const noexcept { return track < kTracks && which < 2u ? (which == 0u ? strips_[track].perf_send_a : strips_[track].perf_send_b) : 0; }
  int step_send(unsigned track, unsigned which) const noexcept { return track < kTracks && which < 2u ? (which == 0u ? strips_[track].step_send_a : strips_[track].step_send_b) : -1; }
  float track_gain(unsigned track) const noexcept { return track < kTracks ? strips_[track].trk_gain : 1.0f; }  // (for tests)
  float track_pan_mod(unsigned track) const noexcept { return track < kTracks ? strips_[track].pan_mod : 0.0f; }
  // Render context (the Engine, a StripMute event: the quantized mute of OS-MUTE reached its boundary): the strip's mute, as the ParamChange Mute sets it (ramp 0: the engine's own smoothing, so the
  // level moves over the block this is called in, before the drain recomputes the strip). The ParamChange path stays for everything else.
  O3OS_AUDIO_HOT void set_strip_mute(unsigned track, bool on) noexcept;
  // The tracks whose input is the caller's feed block (SourceKind::Feed, see feed() below): bit t set = track t. The voices of OS-005d feed the tracks.
  void use_feed_sources(std::uint32_t mask) noexcept;
  // Where a ParamChange goes that names neither a track strip nor the master (the sequencer's tempo and swing, seq_param): the drain offers it here and
  // counts it as rejected unless this says it took it. Render context; installed with the engine's event source.
  using ParamHook = bool (*)(void* self, const ParamChange& change) noexcept;
  void set_param_hook(ParamHook fn, void* self) noexcept {
    param_hook_ = fn;
    param_self_ = self;
  }
  // Where a ParamChange of kind Instrument goes (OS-LIVEP): the engine's descriptors of the sampler voices, Engine::instrument_hook; true when it was a valid one. Without it the change goes
  // to the param hook like the other kinds the graph does not own, which refuses it. Render context; installed with the voices.
  void set_instrument_hook(ParamHook fn, void* self) noexcept {
    inst_hook_ = fn;
    inst_self_ = self;
  }
  bool master_stage_active(MasterStage id) const noexcept {
    const StageSlot& s = master_[static_cast<unsigned>(id)];
    return s.fn != nullptr && !s.bypass;
  }
  // OS-LIMIT, any context (relaxed reads, for `diag mix`): the limiter's gain reduction per meter window; no limiter installed: no windows, no reduction.
  // OS-WAVEXP: the installed limiter's signal state cleared (MasterLimiter::flush), its controls kept; with the render stopped. No limiter: nothing.
  void flush_master_limiter() noexcept {
    if (limiter_ != nullptr) {
      limiter_->flush();
    }
  }
  LimiterReport limiter_report() const noexcept { return limiter_ != nullptr ? limiter_->report() : LimiterReport{0u, 0u, 65536u, 65536u, 0u}; }
  // OS-SENDFX, render context, in flash (O3OS_AUDIO_COLD: the Engine calls it when the tempo in force CHANGED, never per block): the tempo, BPM x 100, to the send effects that want it (the
  // delay's synced times; sendfx::kTempoField).
  O3OS_AUDIO_COLD void set_send_fx_tempo(std::uint32_t tempo_x100) noexcept;
  // OS-SENDFX, render context, in flash (O3OS_AUDIO_COLD: per note start, at most 16 a block): the instrument's DELAY SEND and REVERB SEND, 0..100, of the note that began on `track`. They are the
  // instrument's own sends: added to the strip's own SendA / SendB (the mixer's, 0 unless asked for) and NOT gated by the routing bit (the default route of a track has neither SND bit,
  // and an OG project's sends must sound); the master DELAY / REVERB levels and mutes, the track's mute and a solo elsewhere still close them. `ramp` as ParamChange::ramp_samples: 1 (instant)
  // at a note's start, 0 (the engine's smoothing) for a live edit. master::VoiceSendsReset (a ParamChange) puts them all back to 0 (and ends the step FX's held sends). OS-PERFFX P3: a step FX 30 / 32
  // held replaces these, and the Perform layer's offset is added to them: set_step_send, set_perf_send, below.
  O3OS_AUDIO_COLD void set_voice_sends(unsigned track, unsigned delay, unsigned reverb, std::uint32_t ramp = 1) noexcept;
  O3OS_AUDIO_COLD void reset_voice_sends() noexcept;
  // Are the sends taken after the fader (the project's SENDS setting, PAR-024, as the ParamChange Project / SendsMode sets it) or before it (our default; the official firmware's is an open research question). Before start, or the ParamChange.
  O3OS_AUDIO_COLD void set_sends_post_fader(bool post) noexcept;  // in flash: per ParamChange of that setting
  bool sends_post_fader() const noexcept { return sends_post_; }
  // OS-METRO, render context (the Engine, a Metro event): a beat of the metronome `offset` frames into the block about to be rendered; `accent` is the first beat of a bar. The click is added to the TX block at
  // the end of render(), after every tap (metro.h), to the line out's CUE side, or to MAIN's: OUT MODE Stereo has no CUE output, so there it plays on MAIN whatever METRO says. The mode and level are
  // the ParamChanges master::MetroMode and MetroLevel.
  O3OS_AUDIO_INLINE void metro_beat(std::uint32_t offset, bool accent) noexcept { metro_.trigger(offset, accent); }
  const Metro& metro() const noexcept { return metro_; }
  // OS-LOOPER L1 (loop_player.h): the loop of a take made with LOOP = N BARS, added to the TX block after the taps like the click (so in no take, stem or meter), at LOOP VOL times the master level. The
  // engine hands it the sequencer's Step and Stopped events; the main loop announces, releases and sets it. Routing: ADR 0013 amendment, OS-LOOPER.
  LoopPlayer& loop() noexcept { return loop_; }
  const LoopPlayer& loop() const noexcept { return loop_; }
  // Does a solo silence the other tracks' sends (true: the live mixer, see "Solo" above) or only their amplifiers (false: the export's half).
  void set_solo_mutes_sends(bool on) noexcept {
    solo_mutes_sends_ = on;
    for (unsigned i = 0; i < kStrips; ++i) {
      recompute_strip(i, 1);
    }
  }

  // ---- Main loop ---------------------------------------------------------------------------------------------------------
  // The one producer of the ParamChange ring.
  ParamRing& params() noexcept { return params_; }
  std::uint32_t params_applied() const noexcept { return applied_.load(std::memory_order_relaxed); }    // changes drained
  std::uint32_t params_rejected() const noexcept { return rejected_.load(std::memory_order_relaxed); }  // ... that named nothing or were out of range
  // The feed of a track whose source is SourceKind::Feed: `block` is kBlock samples, 4-byte aligned, that the caller owns and may rewrite between
  // renders (the graph copies it into its scratch each block; null: silence). What the voices of OS-005d will be to a track, a test is to this one.
  void feed(unsigned track, const std::int16_t* block) noexcept {
    if (track < kTracks) {
      feed_[track].store(block, std::memory_order_release);
    }
  }

  // OS-STEREO (c): the side signal S (L = M + S, R = M - S) of the track's stereo voice, kBlock samples, 4-byte aligned, that the caller owns until the next render, next to feed()'s M (null: the track is
  // mono). S follows the strip's fader ramp and takes the strip's pan law into MAIN only (the track's own outputs, its sends, the CUE and REC taps and the USB stems read M); a muted or closed strip, a strip
  // with an insert in the path or one that is not a feed source takes none. Render context (Engine::play_block).
  void feed_side(unsigned track, const std::int16_t* block) noexcept {
    if (track < kTracks) {
      side_[track].store(block, std::memory_order_relaxed);  // (g) relaxed: the writer (Engine::play_block) and the reader (run_tracks) are one render context in order, nothing is published to another; acquire cost a dmb a track
    }
  }

  // The taps and who may ask for them. A tap costs nothing while nobody consumes it: the UI turns the scope and the spectrum on while a screen shows
  // them, and chooses which meters are measured (a bit per channel of meter_ch::; all on by default, so `diag mix` and a mixer screen need nothing).
  const MeterBank& meters() const noexcept { return meters_; }
  void set_meter_mask(std::uint32_t mask) noexcept { meter_mask_.store(mask, std::memory_order_relaxed); }
  std::uint32_t meter_mask() const noexcept { return meter_mask_.load(std::memory_order_relaxed); }
  ScopeRing& scope() noexcept { return mem_.scope; }
  void set_scope_enabled(bool on) noexcept { scope_on_.store(on, std::memory_order_relaxed); }
  bool scope_enabled() const noexcept { return scope_on_.load(std::memory_order_relaxed); }
  SpectrumRing& spectrum() noexcept { return mem_.spectrum; }
  void set_spectrum_enabled(bool on) noexcept { spectrum_on_.store(on, std::memory_order_relaxed); }
  bool spectrum_enabled() const noexcept { return spectrum_on_.load(std::memory_order_relaxed); }
  StemTap& stems() noexcept { return stems_; }
  RecTap& rec() noexcept { return rec_; }
  // OS-H2a: the apps' audio tap (app_tap.h). `tap` lives where the platform places it (SDRAM on the unit); null = none in this build (the default). Before start only. The main loop selects the
  // source (input / master / off) on the object itself; the render writes it from run_taps() while a source is selected, and costs one relaxed load a block when it is off.
  void attach_app_tap(AppTap* tap) noexcept { app_tap_ = tap; }
  AppTap* app_tap() const noexcept { return app_tap_; }
  // USB RETURN (OS-006d, ADR 0012 Decision 4; OS-006e): the host's stereo stream, which reaches the outputs (not MAIN, not any capture) by a route of its own at its own level (see "USB return" above). The reader is
  // the render's end of the return ring (usb::ReturnReader: silence while priming and after an underrun, both counted); `attach_return` is for before start, `set_return_enabled`
  // is flipped by the USB interrupt while the OUT alternate setting is 1. A disabled return costs one atomic load per block.
  // TODO(OS-006e follow-up): CUE and REC bits for the return, and a loopback setting (device row 17), when the Routing screen has a matrix row for it.
  void attach_return(usb::ReturnReader* reader) noexcept {
    return_reader_ = reader;
    return_pull_ = &Graph::pull_return;
  }
  void set_return_enabled(bool on) noexcept { return_on_.store(on, std::memory_order_relaxed); }
  bool return_enabled() const noexcept { return return_on_.load(std::memory_order_relaxed); }
  // OS-011c: what the input strip is made of (graph_params.h InputFeed): the codec (Sum, the default, and CodecOnly) or the USB return alone (silence while no host streams); the return's
  // own route to the outputs is shut in the second case. Main loop, an atomic read once a block; the change lands at the next block's boundary. The return is pulled whatever the feed, so its
  // ring never overruns.
  void set_input_feed(InputFeed feed) noexcept { input_feed_.store(static_cast<std::uint8_t>(feed), std::memory_order_relaxed); }
  InputFeed input_feed() const noexcept { return static_cast<InputFeed>(input_feed_.load(std::memory_order_relaxed)); }

  // ---- Render context --------------------------------------------------------------------------------------------------------
  // One block. `io.in` is the RX block, `io.out` the TX block it fills completely; the stage cycles go to `io.report` (the engine's eight).
  // kFlagSilence (the underrun mute): no source and no input is rendered, the output is digital silence, the test sources stay on their
  // timelines and fade in again afterwards.
  O3OS_AUDIO_HOT void render(const hal::audio::BlockIo& io) noexcept;

  // The test sources of the tracks (SourceKind::Sine / Noise): the engine's HW-A tone script drives them.
  O3OS_AUDIO_HOT void script_tone(unsigned track, bool on) noexcept;
  const TestTone& tone(unsigned track) const noexcept { return strips_[track < kTracks ? track : 0u].tone; }

  // The values the render sees, for tests and `diag`: a plain word the render writes, read for display (it may be a block stale; nothing decides on it).
  std::int32_t level_gain(unsigned strip) const noexcept { return strips_[strip < kStrips ? strip : 0u].level.current(); }
  const LevelCurve& level_curve() const noexcept { return curve_; }
  // OS-ODD11 (the voice gate): the tracks whose strip was inaudible at the end of the last run_tracks (bit t = track t, tracks 0..kAuditionTrack-1): the strip is active and its level and both
  // send gains are closed in the smoothers (a mute, a solo elsewhere, a fader at 0 with shut sends), never the mute flag. Render context only (the render is the one writer and reader).
  std::uint16_t quiet_tracks() const noexcept { return quiet_; }
  void clear_quiet() noexcept { quiet_ = 0; }  // the gate stays open for one block: the export's (Engine::render_offline) and the first live one after it

 private:
  struct Insert {
    InsertFn fn = nullptr;
    void* state = nullptr;
    bool bypass = true;
    // OS-INSFX S1 (render context, written by InsertHost::begin_block through set_insert_wet; in the padding: sizeof(Strip) is unchanged): the wet share, 0..255, at the start and at the end of the
    // block in force. The insert's own dry/wet ramp is the host's (run()); these two only tell side_pass how fast the stereo S of a voice leaves (wet 0 to 255: S 255 to 0) and comes back.
    std::uint8_t wet0 = 255;
    std::uint8_t wet1 = 255;
  };
  struct StageSlot {
    MasterFn fn = nullptr;
    void* state = nullptr;
    bool bypass = true;
  };
  struct FxSlot {
    SendFxFn fn = nullptr;
    void* state = nullptr;
    bool bypass = true;
    SendFxParamFn param = nullptr;
  };
  static constexpr unsigned kDests = 3;  // MAIN, CUE, REC: the buses that take a pan law
  struct Strip {
    bool active = false;
    SourceKind source = SourceKind::None;
    std::uint8_t route = route::kMain;
    bool mute = false;
    bool solo = false;
    std::int8_t perf_gain = 0;  // OS-PERFFX P0, P2: the Perform layer's VOLUME and PANNING values, -100..100, 0 none (set_perf_gain, perf_save); composed in recompute_strip. In the padding before `volume`: no byte of SRAM_U, no offset moves
    std::int8_t perf_pan = 0;
    std::uint8_t xf_gain = 255;  // OS-DJ A1: the DJ crossfader's cut, 0..255, 255 none (graph_param::strip::XfGain); composed in recompute_strip. The last padding byte before `volume`: no byte of SRAM_U
    float volume = 100.0f;
    float pan = 50.0f;
    float pan_offset = 0.0f;  // the note's instrument pan - 50 (set_voice_pan): the pan law gets pan + pan_offset, clamped by the law
    float pan_mod = 0.0f;     // OS-COREFX CF3, the tracker's panbrello (set_track_pan_mod): added to the pan the law gets
    float trk_gain = 1.0f;    // ... and its channel volume x global volume (set_track_gain): a multiplier on the fader
    float send_a = 0.0f;
    float send_b = 0.0f;
    // The instrument's sends at the note that began on the track (set_voice_sends, 0..100), added to send_a / send_b; OS-PERFFX P3: the step FX 30 / 32 held (0..100 in place of the instrument's,
    // -1 none) and the Perform layer's DELAY / REVERB (-100..100, 0 none), composed in recompute_strip. Bytes, not floats: the four new fields cost the strip nothing.
    std::uint8_t inst_send_a = 0;
    std::uint8_t inst_send_b = 0;
    std::int8_t step_send_a = -1;
    std::int8_t step_send_b = -1;
    std::int8_t perf_send_a = 0;
    std::int8_t perf_send_b = 0;
    Smoother level;
    Smoother send_gain[2];
    Smoother dest_l[kDests];  // the pan law's left gain into MAIN, CUE, REC (a closed route is 0); the input strip: unity or 0
    Smoother dest_r[kDests];
    TestTone tone;
    std::uint32_t noise = 1;
    Insert insert[2];
    bool live = false;     // this block: the source produced a block
    bool tap = false;      // this block: the post-fader block is not silence (it was metered, staged for the stems, summed)
    bool settled = true;   // every smoother is at its target (nothing was run since the last snap): a silent strip need not look again
  };
  // OS-PERFFX P0: the two perf bytes sit in the padding before `volume`, so no byte of the SRAM_U strip row and no offset moves (the size is the target's: its pointers are four bytes).
  static_assert(offsetof(Strip, volume) == 8, "perf_gain / perf_pan fill the padding before volume");
  static_assert(sizeof(void*) != 4 || sizeof(Strip) == 216, "the strip row of the SRAM_U budget does not grow");
  // A bus: whether anything was added to its accumulators this block, and whether its saturated plane is not all zero (a plane that was live
  // last block is zeroed once, then left alone). MAIN also has the centre accumulator.
  struct Bus {
    bool dirty = false;
    bool centre = false;  // MAIN only: something was added to the centre accumulator
    bool plane = false;
  };
  // A track that opens MAIN and waits for a partner (or the end of the tracks) to go through the strip kernel.
  struct Pending {
    unsigned track;
    std::int16_t* block;
    kernel::StripLane lane;
    bool centre;
  };

  // OS-STEREO (c): the side signal of `track`'s stereo voice into MAIN's two sides (flash: one call per stereo track and block, hot.h); `lane` is the track's strip lane of this block (level and pan ramps).
  O3OS_AUDIO_COLD void side_pass(unsigned track, const std::int16_t* side, const kernel::StripLane& lane) noexcept;

  // Render-context helpers (placed with render(), in SRAM_L).
  O3OS_AUDIO_HOT void drain() noexcept;
  O3OS_AUDIO_HOT void apply(const ParamChange& c) noexcept;
  // In flash (O3OS_AUDIO_COLD, the exception list of hot.h): per parameter change, once per ParamChange popped by drain() (at most kMaxParamsPerBlock a block,
  // normally none) and once per strip marked dirty (a changed parameter, a note's pan offset, a solo or master change marks all kStrips = 17) at the start of a block: never per sample.
  O3OS_AUDIO_COLD bool apply_strip(unsigned slot, unsigned field, float v, std::uint32_t ramp) noexcept;
  O3OS_AUDIO_COLD bool apply_master(unsigned field, float v, std::uint32_t ramp) noexcept;
  O3OS_AUDIO_COLD bool apply_limiter_mode(float v) noexcept;  // OS-LIMIT: the project's LIMITER MODE, once per change
  O3OS_AUDIO_COLD void recompute_strip(unsigned slot, std::uint32_t ramp) noexcept;
  O3OS_AUDIO_COLD void recompute_pan(unsigned slot, std::uint32_t ramp) noexcept;  // OS-CPU10 B1: a track strip's pan gains alone (drain, when the pan is the only change)
  O3OS_AUDIO_COLD void retarget_pan(Strip& s, bool is_input, std::uint32_t ramp) noexcept;
  // The dirty marks. mark_pan_dirty is the pan setters' (set_voice_pan, set_track_pan, set_track_pan_mod; tracks only): a strip not yet dirty is marked pan-only (pan_only_ is a subset of dirty_); every other
  // mark (mark_dirty, mark_all_dirty) clears the strip's pan-only bit, so a strip is pan-only only while the pan is all that changed since its last recompute. Render context and main thread of the
  // engine, as dirty_ (single writer: the render context).
  O3OS_AUDIO_INLINE void mark_dirty(unsigned slot) noexcept {
    dirty_ |= 1u << slot;
    pan_only_ = static_cast<std::uint16_t>(pan_only_ & ~(1u << slot));
  }
  O3OS_AUDIO_INLINE void mark_all_dirty() noexcept {
    dirty_ = (1u << kStrips) - 1u;
    pan_only_ = 0;
  }
  O3OS_AUDIO_INLINE void mark_pan_dirty(unsigned track) noexcept {
    if (((dirty_ >> track) & 1u) == 0u) {
      pan_only_ = static_cast<std::uint16_t>(pan_only_ | (1u << track));
    }
    dirty_ |= 1u << track;
  }
  O3OS_AUDIO_COLD void set_perf(unsigned track, std::int8_t Strip::*field, int lo, int value, std::uint32_t ramp) noexcept;  // set_perf_gain / set_perf_pan / set_perf_send / set_step_send: clamp to lo..100, store a changed value, mark dirty
  O3OS_AUDIO_COLD void recompute_master(std::uint32_t ramp) noexcept;
  // In flash (O3OS_AUDIO_COLD): once per change of the input feed between ReturnOnly and the others (a recorder source change), never per block: the USB return's route ramps shut or open over a block.
  O3OS_AUDIO_COLD void set_usb_shut(bool shut) noexcept;
  O3OS_AUDIO_HOT void settle(Strip& s) noexcept;
  O3OS_AUDIO_HOT void run_tracks(bool silence) noexcept;
  O3OS_AUDIO_HOT void flush_main(Pending* p, unsigned n) noexcept;
  O3OS_AUDIO_HOT void finish_track(unsigned track, std::int16_t* block) noexcept;
  O3OS_AUDIO_HOT void run_input(bool silence) noexcept;
  O3OS_AUDIO_HOT void add_return(InputFeed feed) noexcept;
  O3OS_AUDIO_HOT void run_buses() noexcept;
  O3OS_AUDIO_HOT void run_chain() noexcept;
  O3OS_AUDIO_HOT void meter_buses() noexcept;
  O3OS_AUDIO_HOT void run_levels() noexcept;
  O3OS_AUDIO_HOT void run_taps(const std::int16_t* out_block) noexcept;
  // What is added to the TX block after the taps (OS-METRO's click, metro.h: the OUT MODE side and the muted render are decided there; OS-LOOPER's loop, loop_player.h), then the cycle count for render()'s
  // stage accounting. In flash (O3OS_AUDIO_COLD): once a block while a click sounds (11 blocks of every beat at 120 BPM) or a loop exists or is being captured, so that the render in SRAM_L pays one test of
  // both (metro_.busy() || loop_.needs_service()) and one call, not the OUT MODE test and the arguments.
  O3OS_AUDIO_COLD std::uint32_t mix_extras(std::int16_t* out, bool silence, std::uint32_t block_start) noexcept;  // block_start: the cycle count at the start of render() (the restore's load guard)
  O3OS_AUDIO_HOT void finish_bus(Bus& bus, std::int16_t (*planes)[kBlock], std::int32_t (*acc)[kBlock], unsigned channels) noexcept;
  // The USB return's gain target (render context, in the header so that the hot render can retarget it): its level, or 0 while the input strip carries the stream.
  std::int32_t usb_target() const noexcept { return usb_shut_ ? 0 : curve_.q16(m_usb_level_); }
  bool chain_active() const noexcept {
    bool on = false;
    for (unsigned i = 0; i < kMasterStages; ++i) {
      on = on || (master_[i].fn != nullptr && !master_[i].bypass);
    }
    return on;
  }
  void init_defaults() noexcept;
  // OS-DJ A2: the DJ filter has a target or a glide to run, so its stage is switched on (only when the slot is still its own).
  O3OS_AUDIO_COLD void dj_engage() noexcept;
  // OS-DJ A3: the same for the kill EQ (the eq slot).
  O3OS_AUDIO_COLD void kill_engage() noexcept;

  CycleFn cycles_;
  TapMem& mem_;

  ParamRing params_;
  std::atomic<std::uint32_t> applied_{0};
  std::atomic<std::uint32_t> rejected_{0};
  std::uint32_t dirty_ = 0;           // strips whose targets must be recomputed
  std::uint32_t ramp_[kStrips] = {};  // the ramp each asked for
  bool any_solo_ = false;
  bool solo_mutes_sends_ = true;
  bool sends_post_ = false;  // OS-SENDFX: the sends follow the fader (the project's SENDS setting)

  ParamHook param_hook_ = nullptr;
  void* param_self_ = nullptr;

  LevelCurve curve_;
  Strip strips_[kStrips];
  std::uint16_t quiet_ = 0;  // OS-ODD11: see quiet_tracks(); render only
  std::uint16_t pan_only_ = 0;  // OS-CPU10 B1: the dirty tracks whose pan is the only change (see mark_pan_dirty); in the padding after quiet_
  static_assert(kTracks <= 16, "pan_only_ has a bit per track");
  FxSlot fx_[2];
  StageSlot master_[kMasterStages];
  // master state
  float m_volume_ = 100.0f;
  float m_dry_ = 100.0f;
  float m_reverb_ = 100.0f;  // SNDB's level
  float m_delay_ = 100.0f;   // SNDA's
  float m_cue_ = 100.0f;
  bool m_dry_mute_ = false;
  bool m_reverb_mute_ = false;
  bool m_delay_mute_ = false;
  bool m_limiter_ = true;
  OutMode out_mode_ = OutMode::Stereo;
  Metro metro_;  // OS-METRO: a few words, mixed into the TX block after the taps
  std::uint8_t ret_route_[2] = {route::kMain, route::kMain};
  Smoother master_gain_;
  Smoother cue_gain_;
  Smoother ret_gain_[2][2];  // return A / B into MAIN / CUE

  // The working blocks, in SRAM_U. int16 planes are 4-byte aligned (declared), accumulators are words.
  alignas(8) std::int16_t scratch_[2][kBlock] = {};  // the two shared track blocks: a track's source, then (in place) its post-fader block
  alignas(8) std::int16_t zero_[kBlock] = {};        // silence, for an effect whose input is closed
  alignas(8) std::int16_t in_[2][kBlock] = {};       // the RX block as planes, then (in place) after the input level
  alignas(8) std::int32_t acc_main_[2][kBlock] = {};
  alignas(8) std::int32_t acc_main_c_[kBlock] = {};  // MAIN's centre accumulator: tracks panned at the centre, added to both sides at the saturation
  alignas(8) std::int32_t acc_cue_[2][kBlock] = {};
  alignas(8) std::int32_t acc_rec_[2][kBlock] = {};
  alignas(8) std::int32_t acc_snd_[2][kBlock] = {};  // SNDA and SNDB, mono
  alignas(8) std::int16_t pl_main_[2][kBlock] = {};  // the buses after their one saturation
  alignas(8) std::int16_t pl_cue_[2][kBlock] = {};
  alignas(8) std::int16_t pl_rec_[2][kBlock] = {};
  alignas(8) std::int16_t pl_snd_[2][kBlock] = {};     // the send buses, mono
  alignas(8) std::int16_t pl_ret_[2][2][kBlock] = {};  // the effects' returns, stereo
  alignas(8) std::int16_t pl_out_[2][kBlock] = {};     // OUT MODE's own pair (Stereo uses pl_main_ in place)
  alignas(8) std::int16_t mono_[kBlock] = {};          // scratch: the input's mono send, the spectrum's mono sum, the app tap's mono block
  alignas(8) float fl_[2][kBlock] = {};                // the master chain's float planes; fl_[0] doubles as the sources' float block
  const std::int16_t* outp_[2] = {nullptr, nullptr};   // the planes the TX block is made of this block
  Bus b_main_, b_cue_, b_rec_, b_snd_[2];
  bool chain_on_ = false;                // this block: a master stage is active, so MAIN is in the float planes, not saturated
  std::uint32_t meter_now_ = 0;          // this block: the meter mask
  std::uint32_t stem_have_ = 0;          // this block: the tracks whose post-fader block was staged for the stems
  std::uint32_t src_cycles_ = 0;         // this block: cycles spent making sources (the Render stage; the rest of the track pass is Strips)

  std::atomic<const std::int16_t*> feed_[kTracks] = {};
  std::atomic<std::uint32_t> meter_mask_{(1u << kMeterChannels) - 1u};
  std::atomic<bool> scope_on_{false};
  std::atomic<bool> spectrum_on_{false};
  // The reader's pull lives in flash (usb/stream.cpp) and the render in SRAM_L, which may not call flash directly (a long-call veneer: the image check
  // `ramfunc` fails the build), so the call goes through a pointer, as the interleave kernels do.
  using ReturnPullFn = bool (*)(usb::ReturnReader*, std::int16_t*, std::int16_t*, std::uint32_t) noexcept;
  static bool pull_return(usb::ReturnReader* reader, std::int16_t* left, std::int16_t* right, std::uint32_t frames) noexcept {
    return reader->pull(left, right, frames);
  }
  usb::ReturnReader* return_reader_ = nullptr;  // before start
  ReturnPullFn return_pull_ = nullptr;
  std::atomic<bool> return_on_{false};
  std::atomic<std::uint8_t> input_feed_{0};  // InputFeed

  ParamHook inst_hook_ = nullptr;  // OS-LIVEP (at the end: the offsets of the members the render reads do not move)
  void* inst_self_ = nullptr;
  MasterLimiter* limiter_ = nullptr;  // OS-LIMIT: the limiter in the chain's slot (install_master_limiter), which the limiter ParamChanges go to
  bool main_pre_ = false;             // this block: the MAIN meters were taken before the limiter (run_chain), meter_buses leaves them alone
  AppTap* app_tap_ = nullptr;         // OS-H2a: set before start (attach_app_tap), never changed while the render runs

  MeterBank meters_;
  StemTap stems_;
  RecTap rec_;

  // OS-006e, the USB return's own route (render context; the level reaches it as a ParamChange). At the end with the other later members, so the offsets the render reads do not move.
  float m_usb_level_ = 100.0f;  // the return's level to the outputs, the mixer level law (unity)
  Smoother usb_gain_;           // its gain: the target of m_usb_level_, or 0 while the strip carries the stream (usb_target); a ramp over a block on a change
  bool usb_shut_ = false;       // the input feed is ReturnOnly: the route is ramping or held shut
  bool ret_live_ = false;       // this block: ret_ holds the host's stream (no host streaming: false)
  bool ret_heard_ = false;      // this block: the stream is added to the heard planes (run_levels, after the master level)
  alignas(8) std::int16_t ret_[2][kBlock] = {};  // the host's stream of this block, as planes (add_return)

  LoopPlayer loop_;  // OS-LOOPER L1: a few words (loop_player.h), mixed into the TX block after the taps

  MasterScratch scratch_stage_;  // OS-DJ C1: the `headroom` slot's processor (init_defaults installs it, bypassed); the ScratchMode ParamChange engages it, and it bypasses itself when its release has ended
  MasterKillEq kill_eq_;  // OS-DJ A3: the `eq` slot's processor (init_defaults installs it, bypassed); the KillEq ParamChange engages it, and it bypasses itself once no kill is on and the gains are back at 1
  MasterDjFilter dj_filter_;  // OS-DJ A2: the `bass` slot's processor (init_defaults installs it, bypassed); the DjFilter ParamChange engages it, and it bypasses itself once it has run down to 0
  // OS-STEREO (c): the side signal S of a stereo voice per track (feed_side), at the end like the members above. Written by the Engine (render context) once a block, read by run_tracks; null: the track is mono.
  std::atomic<const std::int16_t*> side_[kTracks] = {};
};

}  // namespace o3os::audio
