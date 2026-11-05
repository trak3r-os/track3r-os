// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The sample recorder (OS-011c): the model behind the Record screen. What it does, in the order a take lives:
//
//   SOURCE      the input the take is made of: line in (mixed, left, right), the microphone (two gain ranges), the FM radio, the USB return; or, on the RESAMPLE page,
//               what the engine itself plays (the master, one track, the REC bus). The analogue part goes to the board through hal::capture::CaptureHw (the codec's input switch
//               and gain, the radio); the rest is the engine's: the input strip's feed (audio::InputFeed: the codec, the USB return, or the sum), the REC tap's channel (mix, left,
//               right) and source (the REC bus, or MAIN after the master level), and the routing of the strips (which strips reach the REC bus).
//   MONITOR     the input is heard through the engine, never through the codec's own loop: the input strip
//               goes to MAIN and CUE (audio/graph.h "Monitoring"); the pattern keeps playing, the render does not look at the recorder's state. OFF by default (a microphone and a
//               speaker feed back). Whatever the monitor does, the take is recorded: REC is a route of the strip of its own, taken before nothing but the
//               level, which the recorder holds at unity while it owns the input (and at zero for 200 ms after a source change: the ADC's input switch clicks).
//   ARM / REC   a take is written by the render straight into the sample arena's free tail (SampleBank::take_begin: the tail is held, the take's length is "as much as is free"),
//               from a step of the pattern if one was asked for: Free (now), Step, Bar (every 16th step) or Pattern (step 0), to the SAMPLE, so that the take lines up with the
//               timeline (RecTap in audio/taps.h). The aligned modes fall back to Free when the transport is stopped (there is no step to wait for). STOP, a full arena, or STOP AT (OS-REC2: the
//               next step, bar or pattern start, or N bars after the take began, to the sample on the same timeline: RecStop below) ends it.
//   COUNT-IN (OS-RECCOUNT, RC1; NEW COMMUNITY OS BEHAVIOUR). With the Record card's COUNT-IN row on (OFF, 1, 2, 4 BARS) a bar-aligned take (LOOP = N BARS, or START ON = BAR) counts
//               N bars in before it records: from stop the transport is started at once (plain play, not Config's count-in) and the take records from the first step of bar N+1; with the
//               transport running it waits for the next bar boundary, then N more bars, and records from there. The wait runs here on the main loop (count_in_poll): the tap is armed during
//               the LAST count-in bar, so its Align::Bar picks the boundary after the N bars (nothing under audio/ is involved). A Stop or F1 during the count-in gives the take up. Never
//               for FREE, STEP, PATTERN, LEVEL, and never for the OVERDUB (that keeps Config's COUNT-IN, QA-REC-032). OFF is today's behaviour exactly.
//   REVIEW      the finished take lies in the arena: its overview is drawn (take_overview.h), its start and end can be trimmed, and SAVE makes the window the audio of an
//               instrument slot (SampleBank::take_commit: fresh memory, handed to the voices at the next safe point; nothing a voice reads is ever rewritten). Saving a WAV to the card
//               needs the SD write layer, which this tree has not got (storage is read only): the Record screen says so.
//
// LOOP (OS-LOOPER L1, docs/specs/OS-LOOPER.md; NEW COMMUNITY OS BEHAVIOUR). With LOOP = N BARS the take is a bar-aligned take that ends by itself after exactly N bars (ALIGN is BAR and STOP AT is LOOP whatever those rows
//               say; the transport must run: F1 from stop starts it, after the count-in when COUNT-IN is on), and when it ends the render goes on playing it, clocked to the bars (audio/loop_player.h, which
//               documents the clock and the routing): REVIEW is then the LOOP REVIEW, with LOOP VOL in place of the trim rows, F8 muting the loop, F3 saving it as an instrument that loops (play mode LOOP
//               FWD, exactly N bars) and F4 throwing it away. A loop take that ends any other way (F1 before the Nth bar, the transport stopped, the arena full) is not a loop: it is given up with a note.
//               The loop reads the take where it lies in the arena; nothing writes that memory until the render has let go (arm() waits for LoopPlayer::phase() Idle after a DISCARD; a SAVE hands it to the slot).
//
// WHAT THE RECORDER OWNS. While the Record screen is in view, or a take is armed or recording (it goes on when the user leaves), the recorder owns the engine's input path: the
// input strip's level and route, the feed, the REC tap's source and channel and the REC bit of every strip's route. It reads the Mixer's own values from the MixBoard (the board
// holds what the app last asked of the engine) and gives them back when it lets go: nothing the user set on the Mixer is changed, and the Mixer screen shows the board.
//
// Real-time: nothing here runs in the render. The engine is told by ParamChanges (a full ring is retried at the next poll) and by the graph's atomics; the render writes the take
// and publishes how much of it is written (RecTap::frames(), acquire); this reads it. No heap.
//
// Contexts: main loop only (poll() once a frame of the app, the actions between).

#include <o3os/app/mix_board.h>
#include <o3os/app/record_view.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/take_overview.h>
#include <o3os/audio/graph.h>
#include <o3os/core/param.h>
#include <o3os/hal/capture.h>
#include <o3os/ui/plot/peak_cache.h>

#include <cstdint>

namespace o3os::app {

// ---- Sources ----------------------------------------------------------------------------------------------------------------------
struct SourceInfo {
  const char* name;           // up to 10 characters, upper case (the screen's list)
  audio::RecTap::Channel channel;  // which channel of the REC bus a mono take is made of
  audio::InputFeed feed;      // what the input strip is made of while this is the source
  std::uint8_t gain_bank;     // which gain the source uses (kGainBanks), or kNoGain
};
inline constexpr std::uint8_t kGainBanks = 4;  // line in, microphone low range, microphone high range, radio: this OS keeps these four apart
inline constexpr std::uint8_t kNoGain = 0xFF;
// By hal::capture::Source. Our model: a line is recorded as the mean of its two channels and a microphone as the
// LEFT channel; line left and right alone, and USB, are further sources.
inline constexpr SourceInfo kSources[hal::capture::kSourceCount] = {
    {"LINE", audio::RecTap::Channel::Mix, audio::InputFeed::CodecOnly, 0},
    {"LINE L", audio::RecTap::Channel::Left, audio::InputFeed::CodecOnly, 0},
    {"LINE R", audio::RecTap::Channel::Right, audio::InputFeed::CodecOnly, 0},
    {"MIC LG", audio::RecTap::Channel::Left, audio::InputFeed::CodecOnly, 1},
    {"MIC HG", audio::RecTap::Channel::Left, audio::InputFeed::CodecOnly, 2},
    {"RADIO", audio::RecTap::Channel::Mix, audio::InputFeed::CodecOnly, 3},
    {"USB", audio::RecTap::Channel::Mix, audio::InputFeed::ReturnOnly, kNoGain},
};
constexpr const SourceInfo& source_info(hal::capture::Source s) noexcept {
  return kSources[static_cast<unsigned>(s) < hal::capture::kSourceCount ? static_cast<unsigned>(s) : 0u];
}

// ---- What the engine is told ----------------------------------------------------------------------------------------------------------------
enum class RecTapKind : std::uint8_t { Master = 0, RecBus, Track };  // the RESAMPLE page's taps
inline constexpr unsigned kRecTapKinds = 3;
enum class RecAlign : std::uint8_t { Free = 0, EachStep, Bar, Pattern, Level };
inline constexpr unsigned kRecAligns = 5;
inline constexpr const char* kRecAlignNames[kRecAligns] = {"FREE", "STEP", "BAR", "PATTERN", "LEVEL"};

// START ON LEVEL (OS-SREC-LEVEL, E-SREC-11). NEW COMMUNITY OS BEHAVIOUR: the take starts when the tapped bus (the REC bus, MAIN or the track the page taps; not the codec input alone) reaches the THRESHOLD, with the PRE-ROLL
// before it kept (audio/taps.h RecTap::arm_level). It needs no timeline (it works with the transport stopped; LOOP is on the bars and never uses it). THRESHOLD -60..-6 dB in steps of 3 (default -30), PRE-ROLL
// 0 / 10 / 20 / 50 / 100 ms (default 50). Runtime only: nothing here is stored. The pre-roll is kept at least: it is rounded up to an even frame and one frame more may be kept to start the take on one.
inline constexpr int kRecLevelMinDb = -60;
inline constexpr int kRecLevelMaxDb = -6;
inline constexpr int kRecLevelStepDb = 3;
inline constexpr int kRecLevelDefaultDb = -30;
inline constexpr unsigned kRecPreRolls = 5;
inline constexpr unsigned kRecPreRollDefault = 3;
inline constexpr std::uint8_t kRecPreRollMs[kRecPreRolls] = {0, 10, 20, 50, 100};
// The int16 magnitude a frame must reach for `db` (the nearest multiple of 3 inside the range): 32768 x 10^(db / 20), from a table so that nothing needs a log.
constexpr std::int32_t rec_level_threshold(int db) noexcept {
  constexpr std::int32_t kTable[19] = {33, 46, 65, 92, 130, 184, 260, 368, 519, 734, 1036, 1464, 2068, 2920, 4125, 5827, 8231, 11627, 16423};
  const int c = db < kRecLevelMinDb ? kRecLevelMinDb : (db > kRecLevelMaxDb ? kRecLevelMaxDb : db);
  return kTable[(c - kRecLevelMinDb) / kRecLevelStepDb];
}
inline constexpr const char* kRecTapNames[kRecTapKinds] = {"MASTER", "REC BUS", "TRACK"};

// STOP AT (OS-REC2). NEW COMMUNITY OS BEHAVIOUR: a take can end on the sequencer's timeline or by itself. Manual is that.
// Step, Bar and Pattern make the take's stop (F1, or the Rec button) land on the sequencer's timeline: it ends at the SAMPLE of the next step, the next bar (a step index multiple of 16, the
// grid RecAlign::Bar uses) or the next pattern start, to the sample (audio/taps.h RecTap::stop_at); a second F1 while it waits stops it at once. Bars (1..16) ends the take by itself 16 N steps
// after it began (RecTap auto_steps), counted from the step it began at (START ON STEP / BAR / PATTERN: exactly N bars; FREE: from the first step event it sees). All of them need the transport
// to be RUNNING: while it is stopped there is no timeline and STOP AT acts as Manual (the screen shows the field as "BAR (MANUAL)"), and a take that was told to end at a boundary ends where
// the transport stops (RecTap::on_stopped). F1 in Bars mode stops at once (the early end). Runtime only: nothing here is stored (no settings or project change).
enum class RecStop : std::uint8_t { Manual = 0, EachStep, Bar, Pattern, Bars };
inline constexpr unsigned kRecMaxBars = 16;
inline constexpr unsigned kRecStopChoices = 4u + kRecMaxBars;  // MANUAL, STEP, BAR, PATTERN, 1 BAR .. 16 BARS
inline constexpr std::uint32_t kStepsPerBar = 16;              // four steps a beat, four beats a bar: the sequencer's grid (taps.h: Align::Bar)
inline constexpr const char* kRecStopNames[4] = {"MANUAL", "STEP", "BAR", "PATTERN"};
inline constexpr unsigned kRecLoopChoices = 1u + kRecMaxBars;  // OS-LOOPER: LOOP OFF, 1 BAR .. 16 BARS

inline constexpr std::uint32_t kSourceSettleMs = 200;  // after the ADC's input switch changed: 200 ms (this OS's settle time)

// OS-RECCOUNT (RC1): the Record card's own COUNT-IN row (right after LOOP), session-only like the rest of RecorderConfig (not in settings.o3st). Values OFF, 1, 2, 4 BARS
// (a bar is kStepsPerBar steps, the grid Align::Bar uses). It applies to a bar-aligned take only (LOOP = N BARS, or START ON = BAR): from stop F1 starts the transport at once
// (plain play, NOT Config's play_count_in: the row replaces the Config COUNT-IN for that take) and the take records from the first step of bar N+1; with the transport running the
// take waits for the next bar boundary, then N more bars, and records from there. Never to FREE, STEP, PATTERN, LEVEL, and never to OVERDUB (the overdub keeps the Config COUNT-IN,
// QA-REC-032). The N-bar wait runs on the main loop (Recorder::count_in_poll): the take is armed during the LAST count-in bar, so the tap's Align::Bar picks the boundary after the N
// bars; nothing under audio/ is touched. OFF (0, the default) is today's behaviour exactly.
inline constexpr unsigned kRecCountInChoices = 4;
inline constexpr std::uint8_t kRecCountInBars[kRecCountInChoices] = {0, 1, 2, 4};  // OFF, 1 BAR, 2 BARS, 4 BARS
constexpr unsigned rec_count_in_index(std::uint8_t bars) noexcept {
  return bars >= 4u ? 3u : (bars >= 2u ? 2u : (bars >= 1u ? 1u : 0u));
}

struct RecorderConfig {
  hal::capture::Source source = hal::capture::Source::LineMix;
  std::uint8_t gain[kGainBanks] = {0, 0, 0, 0};  // percent, per bank: our defaults (all 0)
  bool monitor = false;
  RecAlign align = RecAlign::Bar;
  std::int8_t level_db = kRecLevelDefaultDb;        // START ON LEVEL: THRESHOLD (runtime only)
  std::uint8_t preroll = kRecPreRollDefault;        // START ON LEVEL: PRE-ROLL, an index into kRecPreRollMs (runtime only)
  RecStop stop = RecStop::Manual;  // OS-REC2: where a take ends (runtime only)
  std::uint8_t stop_bars = 1;      // RecStop::Bars: 1..kRecMaxBars
  std::uint8_t loop_bars = 0;      // OS-LOOPER: LOOP, 0 = OFF, else 1..kRecMaxBars (runtime only)
  std::uint8_t loop_vol = 100;     // LOOP VOL 0..100 (runtime only)
  std::uint8_t loop_fb = 100;      // FEEDBACK 0..100: the part of the loop kept under a layer (OS-LOOPER L2; runtime only; 100 adds, 0 replaces)
  std::uint8_t count_in = 0;       // OS-RECCOUNT: COUNT-IN, 0 = OFF, else an entry of kRecCountInBars (runtime only, session-only)
  RecTapKind tap = RecTapKind::Master;
  std::uint8_t track = 0;  // for RecTapKind::Track: 0..15
  hal::capture::Region region = hal::capture::Region::Europe;
  std::uint16_t radio_freq = 8750;  // our default, 87.5 MHz
};

// Whether the COUNT-IN row applies to a take of this config: a LOOP take, or START ON = BAR (the bar-aligned takes). Not to the others, and never to the overdub (that is not a take of the row).
constexpr bool count_in_applies(const RecorderConfig& c) noexcept { return c.loop_bars != 0u || c.align == RecAlign::Bar; }

// What the engine's input path is told: the input strip's feed, the REC tap's channel and source, the input strip's level, and the route byte of every strip (audio::route bits).
struct EnginePlan {
  audio::InputFeed feed = audio::InputFeed::Sum;
  audio::RecTap::Channel channel = audio::RecTap::Channel::Mix;
  audio::RecTap::Source rec_source = audio::RecTap::Source::Bus;
  std::uint8_t in_level = 0;
  std::uint8_t route[kMixStrips] = {};
};

// The plan for a mode. `owned`: the recorder owns the input path (else the plan is the Mixer's own values: `board`, or the engine's defaults without one). `settling`: a source
// change is less than kSourceSettleMs old: the input is held silent. Pure.
EnginePlan make_plan(const RecorderConfig& cfg, RecMode mode, bool owned, bool settling, const MixBoard* board) noexcept;
// OS-SREC-LEVEL: THRESHOLD and PRE-ROLL are rows only for START ON = LEVEL. A LOOP starts on a bar whatever START ON says (Recorder::arm), so with LOOP on the two rows are no use and are not shown.
constexpr bool level_rows(const RecorderConfig& c) noexcept { return c.align == RecAlign::Level && c.loop_bars == 0u; }
// What the take starts on when the user asked for `align`: the aligned modes fall back to Free while the transport is stopped.
constexpr audio::RecTap::Align effective_align(RecAlign align, bool playing) noexcept {
  if (align == RecAlign::Level) {
    return audio::RecTap::Align::Level;  // (a level needs no timeline: it stays LEVEL while the transport is stopped)
  }
  if (!playing || align == RecAlign::Free) {
    return audio::RecTap::Align::Free;
  }
  return align == RecAlign::EachStep ? audio::RecTap::Align::Step : (align == RecAlign::Bar ? audio::RecTap::Align::Bar : audio::RecTap::Align::Pattern);
}
// OS-METROALIGN: how many steps a take that waits for `align` still has to wait, from the step now running (`step` of a pattern `pattern_len` steps long; the running one is included, as the
// count-in's published steps are): 0 when nothing is waited for. The take begins at the Step event the alignment accepts (RecTap::on_step): the next step, the next multiple of 16 or the pattern's
// step 0, whichever the pattern reaches first. Pure; display only (a Break or a Jump inside the pattern moves the boundary, and the countdown follows the position it reads).
constexpr unsigned wait_steps_left(audio::RecTap::Align align, unsigned step, unsigned pattern_len) noexcept {
  if (align == audio::RecTap::Align::Free || align == audio::RecTap::Align::Level || step >= pattern_len) {
    return 0;
  }
  if (align == audio::RecTap::Align::Step) {
    return 1;
  }
  unsigned boundary = pattern_len;
  constexpr unsigned kBar = kStepsPerBar;
  const unsigned bar = (step / kBar + 1u) * kBar;
  if (align == audio::RecTap::Align::Bar && bar < boundary) {
    boundary = bar;
  }
  return boundary - step;
}
// The beat a wait shows as a count-in does (beats left, the running one included, 4..1 in a bar): a wait of less than one beat is never started (`shown` false), one that has begun goes on to its end.
constexpr unsigned wait_beat(unsigned steps_left, bool shown) noexcept { return steps_left != 0u && (steps_left >= 4u || shown) ? (steps_left + 3u) / 4u : 0u; }
// OS-RECCOUNT: the steps from `step` of a pattern `pattern_len` steps long to its next bar boundary (the running step is not one; the boundary at step 0 after the wrap included). Pure.
constexpr unsigned steps_to_bar_boundary(unsigned step, unsigned pattern_len) noexcept {
  if (pattern_len == 0u || step >= pattern_len) {
    return 0;
  }
  const unsigned next = (step / kStepsPerBar + 1u) * kStepsPerBar;
  return next < pattern_len ? next - step : pattern_len - step;
}
// What STOP AT does while the transport is `playing` or not: without a running timeline there is nothing to count or to wait for, so it is Manual.
constexpr RecStop effective_stop(RecStop stop, bool playing) noexcept { return playing ? stop : RecStop::Manual; }
// The boundary a take told to stop waits for (Free: it stops at once), and the Step events after which a Bars take ends by itself (0: it does not).
constexpr audio::RecTap::Align stop_boundary(RecStop stop) noexcept {
  return stop == RecStop::EachStep ? audio::RecTap::Align::Step : (stop == RecStop::Bar ? audio::RecTap::Align::Bar : (stop == RecStop::Pattern ? audio::RecTap::Align::Pattern : audio::RecTap::Align::Free));
}
constexpr std::uint32_t stop_auto_steps(RecStop stop, unsigned bars, bool playing) noexcept {
  return playing && stop == RecStop::Bars ? kStepsPerBar * (bars < 1u ? 1u : (bars > kRecMaxBars ? kRecMaxBars : bars)) : 0u;
}
// The source the hardware is pointed at in a mode (the RESAMPLE page leaves it where it was).
constexpr hal::capture::Source mode_source(const RecorderConfig& cfg, RecMode mode) noexcept {
  return mode == RecMode::Radio ? hal::capture::Source::Radio : cfg.source;
}
// The REC bus's meter channels of the plan (the left one; the right follows): the bus the take is made of.
constexpr unsigned meter_left(audio::RecTap::Source s) noexcept { return s == audio::RecTap::Source::Master ? audio::meter_ch::kMainL : audio::meter_ch::kRecL; }

class Recorder {
 public:
  enum class State : std::uint8_t {
    Idle,       // no take
    Armed,      // waiting for the step the take starts on
    Recording,  // the render is writing the take
    Review      // a take exists
  };
  enum class ArmResult : std::uint8_t { Ok, NotReady, Settling, NoRoom, Busy };
  // Why a take ended or a request was refused, for the screen's line.
  enum class Note : std::uint8_t { None, Full, Lost, CodecFailed, NoRoom, Settling, LoopCut };

  static constexpr std::uint32_t kFeedBudget = 16384;  // frames of the take the overview reads a poll (the render writes 705 in 16 ms: this is catching up after a long pass)
  static constexpr std::uint32_t kRefillBudget = 65536;  // frames of a loop the overview reads a poll when a session or a restore changed it (OS-LOOPER L2c)
  static constexpr std::uint32_t kMinWindow = 64;      // the smallest window a trim leaves

  struct Wiring {
    hal::capture::CaptureHw* hw = nullptr;
    audio::Graph* graph = nullptr;
    core::ParamRing* params = nullptr;
    SampleBank* bank = nullptr;
    const MixBoard* mix = nullptr;
  };
  void attach(const Wiring& w) noexcept { w_ = w; }
  ~Recorder() {
    if (w_.bank != nullptr && w_.bank->clear_hook_is(this)) {
      w_.bank->set_clear_hook(nullptr, nullptr, nullptr);  // (the bank must not call a recorder that is gone)
    }
  }
  bool ready() const noexcept { return w_.graph != nullptr && w_.bank != nullptr && w_.bank->ready(); }

  RecorderConfig cfg;

  // ---- Once a frame of the app ----
  // `in_view`: the Record screen is shown, `mode` its page, `playing` the transport runs. True when something the screen shows changed (state, time, overview, radio, note). `live_idle`: no live note
  // is queued or held (PreviewSink::live_idle): a thrown-away take gives its room back to the arena only then, since a take pad's note reads it (SampleBank::take_drop).
  bool poll(std::uint64_t now_ms, bool in_view, RecMode mode, bool playing, bool live_idle = true) noexcept;

  // ---- Control (the actions) ----
  // `tempo_x100` (OS-LOOPER): the tempo the bars of a loop take are made at, BPM x 100 (0: unknown): a loop whose N bars at that tempo do not fit the free arena is refused (NoRoom) at once, not given up at the end.
  ArmResult arm(bool playing, std::uint32_t tempo_x100 = 0) noexcept;
  // OS-RECCOUNT: arm a take that counts `count_in` bars in first (the COUNT-IN row applies: the take is bar-aligned). `playing` is the transport as arm() takes it (the caller starts
  // it when it was stopped), `step` and `pattern` the published position at this moment, `from_stop` whether the transport was stopped (its start at step 0 is then the first
  // counted boundary). The tap is
  // NOT armed yet: the count-in phase runs on the main loop (count_in_poll), which arms the take during the LAST count-in bar, so the tap's Align::Bar picks the boundary after the N bars
  // (the take records from the first step of bar N+1 from stop, or from the (N+1)-th boundary with the transport running). A Stop or F1 during the phase gives the take up (stop()).
  ArmResult arm_count_in(bool playing, unsigned count_in, unsigned step, unsigned pattern, bool from_stop, std::uint32_t tempo_x100 = 0) noexcept;
  // OS-RECCOUNT, once a pass, from the App with the published position: `pattern` and `step` of the pattern `pattern_len` steps long now running (pattern_len 0: no position, the
  // phase does not advance), `playing` the transport. Counts the bar boundaries that have come and arms the take during the last count-in bar (never within the last step before the
  // boundary it waits for, unless that last step is all the last count-in bar has). Main loop only.
  void count_in_poll(unsigned pattern, unsigned step, unsigned pattern_len, bool playing) noexcept;
  // The take ends: at the next block, or (OS-REC2) when STOP AT is STEP, BAR or PATTERN, the transport runs and the take is recording, at the sample of the next such boundary of the timeline
  // (stop_pending() is true meanwhile); a second call, a take still waiting for its start step, and every other STOP AT end it at the next block.
  void stop() noexcept;
  void discard() noexcept;   // Review to Idle: the take stays where it is in the arena until the next one reuses it
  // OS-QAFIX-E-SREC-09: the Review take was written out (SAVE WAV started) and its window has not moved since: F4 DISCARD and a new take over it need no question. SAVE (F3) ends the review itself.
  bool take_saved() const noexcept { return state_ == State::Review && saved_; }
  void note_saved() noexcept { saved_ = state_ == State::Review; }
  // The Review take's window [trim_start, trim_end) as audio for slot `slot` (SampleBank::take_commit). False: no take.
  bool commit(unsigned slot) noexcept;
  void set_note(Note n) noexcept { note_ = n; }

  // The source and its gain, the monitor, the alignment, the resample tap (the config, and the hardware at the next poll).
  void step_source(int detents) noexcept;
  void step_gain(int detents) noexcept;       // the current source's gain, 0..100
  void reset_gain() noexcept;
  void toggle_monitor() noexcept { cfg.monitor = !cfg.monitor; dirty_ = true; }
  void step_align(int detents) noexcept;
  void step_threshold(int detents) noexcept;  // OS-SREC-LEVEL: THRESHOLD -60..-6 dB in steps of 3 (the next take)
  void step_preroll(int detents) noexcept;    // OS-SREC-LEVEL: PRE-ROLL 0 / 10 / 20 / 50 / 100 ms (the next take)
  void step_tap(int detents) noexcept;        // MASTER / REC BUS / TRACK n: a detent moves through the 18 choices
  void step_stop(int detents) noexcept;       // OS-REC2: MANUAL / STEP / BAR / PATTERN / 1 BAR .. 16 BARS: a detent moves through the 20 choices
  // OS-LOOPER: LOOP OFF / 1 .. 16 BARS (the next take), LOOP VOL 0..100 (live, heard at the next block), the loop's mute (F8), and the tempo the loop was made at (for the screen's TEMPO CHANGED).
  void step_loop(int detents) noexcept;
  void step_loop_vol(int detents) noexcept;
  void toggle_loop_mute() noexcept;
  void step_loop_fb(int detents) noexcept;
  void step_count_in(int detents) noexcept;  // OS-RECCOUNT: COUNT-IN OFF / 1 BAR / 2 BARS / 4 BARS (the next take)
  // OS-LOOPER L2c (spec 3c): the overdub of a loop under review. All of it is main loop; the render's side is audio/loop_player.h.
  enum class Overdub : std::uint8_t { Started, Stopped, Ignored, Undoing, NoLoop };
  // F1 under a loop's review: begins a session (the bitmap is cleared here, then overdub-on is posted), or ends the running one (its punch-out ramp). Ignored while a punch-out is still being acknowledged or a
  // SAVE waits; Undoing while a restore runs or is about to.
  Overdub overdub_toggle() noexcept;
  // F5: puts the layer back (the session ends first, then the loop is restored a few blocks at a time). False: refused (no layer, a restore under way, a SAVE waiting).
  bool undo_layer() noexcept;
  // F3 with a session on: posts quiesce and waits (SaveWait) for the render to acknowledge that the session has ended and nothing more will be written; the screen keeps drawing the review, every other key
  // is ignored. save_ready() is true when the acknowledgement is in: the caller then commits as in the single step. needs_quiesce(): a session is on or a command is still unacknowledged.
  bool needs_quiesce() const noexcept;
  void begin_save() noexcept;
  // The wait is bounded (kSaveWaitMs of UI time): a render that does not acknowledge (it is not running) must not hold the keys for ever. The caller cancels the wait and says so; the quiesce
  // stays posted (the session is over whenever the render takes it), nothing was committed, and F3 again begins a new wait.
  static constexpr std::uint64_t kSaveWaitMs = 500;
  bool save_timed_out() const noexcept { return save_wait_ && !save_ready() && now_ms_ - save_t0_ >= kSaveWaitMs; }
  bool save_waiting() const noexcept { return save_wait_; }
  bool save_ready() const noexcept { return save_wait_ && w_.graph != nullptr && w_.graph->loop().acked(save_seq_); }
  void cancel_save() noexcept { save_wait_ = false; touch(); }
  bool overdubbing() const noexcept { return loop_review_ && w_.graph != nullptr && w_.graph->loop().overdubbing(); }
  bool restoring() const noexcept { return loop_review_ && w_.graph != nullptr && w_.graph->loop().restoring(); }
  bool layer() const noexcept { return loop_review_ && w_.graph != nullptr && w_.graph->loop().layer(); }
  // A restore runs, or UNDO was posted and the render has not yet acknowledged it.
  bool undo_busy() const noexcept { return restoring() || (undo_seq_ != 0u && w_.graph != nullptr && !w_.graph->loop().acked(undo_seq_)); }
  // The radio (the RADIO page): a detent is a channel; seek; the region.
  void radio_step(int detents) noexcept;
  void radio_set(std::uint16_t freq) noexcept;  // a frequency (snapped to the region's grid)
  void radio_seek(bool up) noexcept;
  void radio_region_step(int detents) noexcept;
  // `diag radio retry` (OS-RAD1 review): may the receiver's power-up be started again now? Restarting puts the chip in reset for about 0.7 s, which a take from the radio would record
  // as silence, and powers it up even when the user picked line or the microphone. The caller (the console, main loop) restarts only on Allowed and otherwise reports the state.
  enum class RadioRetry : std::uint8_t { Allowed, NoRadio, NotSelected, TakeRunning };
  RadioRetry radio_retry() const noexcept {
    if (w_.hw == nullptr) {
      return RadioRetry::NoRadio;
    }
    if (source() != hal::capture::Source::Radio) {
      return RadioRetry::NotSelected;
    }
    return state_ == State::Armed || state_ == State::Recording ? RadioRetry::TakeRunning : RadioRetry::Allowed;
  }
  // The trim of a Review take: the markers in take frames, clamped to leave kMinWindow.
  void set_trim_start(std::uint32_t frame) noexcept;
  void set_trim_end(std::uint32_t frame) noexcept;
  // OS-RECCROP: the take is cut down to its window (the audio outside it is gone). False: no take, a loop, or nothing to cut. The caller makes sure no note reads the take (SampleBank::preview_quiet).
  bool crop() noexcept;
  void step_trim_start(int detents, std::uint32_t unit) noexcept;
  void step_trim_end(int detents, std::uint32_t unit) noexcept;

  // ---- What the screen shows ----
  State state() const noexcept { return state_; }
  Note note() const noexcept { return note_; }
  // OS-METROALIGN: the alignment the armed take waits for (Free: it does not wait: no take, or one already recording), for the countdown of the strip.
  audio::RecTap::Align wait_align() const noexcept { return state_ == State::Armed ? wait_align_ : audio::RecTap::Align::Free; }
  // OS-RECCOUNT: the armed take is in its count-in phase (the tap is not armed yet), and the steps left until the boundary the take starts at (for the strip's countdown, as
  // wait_steps_left is for the plain wait: the beats of the whole N-bar count-in, not only the last bar's).
  bool count_in_waiting() const noexcept { return state_ == State::Armed && count_in_bars_ != 0u; }
  unsigned count_in_steps_left(unsigned step, unsigned pattern_len) const noexcept;
  // OS-RECCOUNT: the count-in was given up because the tap refused the arm for kCiArmMaxTries passes; the App says so once. Consumed on read (main loop only).
  bool count_in_arm_failed() noexcept {
    const bool f = ci_arm_failed_;
    ci_arm_failed_ = false;
    return f;
  }
  // Simulator only (scenario `playhead ... wait`): a recorder with no engine behind it that says it waits. Never called on the device.
  void sim_wait(audio::RecTap::Align a) noexcept {
    state_ = a == audio::RecTap::Align::Free ? State::Idle : State::Armed;
    wait_align_ = a;
  }
  // OS-LOOPER: a loop take is armed or recording (the loop will start when it ends), the loop is under review, the bars it was made with, its mute, the tempo it was made at (BPM x 100; 0: unknown).
  // OS-DJ C1: the arena's free tail is the recorder's: a take is armed, recording or kept (a loop plays from its Review take), or the loop player has not let go of one yet. The scratch ring asks before it takes the tail.
  bool arena_busy() const noexcept { return state_ != State::Idle || (w_.graph != nullptr && w_.graph->loop().phase() != audio::LoopPlayer::Phase::Idle); }
  bool loop_take() const noexcept { return loop_take_; }
  bool loop_review() const noexcept { return state_ == State::Review && loop_review_; }
  unsigned loop_bars() const noexcept { return loop_bars_; }
  bool loop_muted() const noexcept { return w_.graph != nullptr && w_.graph->loop().muted(); }
  std::uint32_t loop_tempo() const noexcept { return loop_tempo_; }
  bool stop_pending() const noexcept { return stop_pending_; }  // F1 was pressed and the take waits for its STOP AT boundary
  bool ends_by_itself() const noexcept { return auto_end_; }    // the take was armed with STOP AT N BARS and the transport running: it ends N bars after it began
  bool owned() const noexcept { return owned_; }
  bool settling() const noexcept { return settling_; }
  bool monitor_on() const noexcept { return cfg.monitor; }
  RecMode mode() const noexcept { return mode_; }
  // The take, so far or finished: frames, the markers, and whether the arena filled.
  std::uint32_t frames() const noexcept { return frames_; }
  std::uint32_t trim_start() const noexcept { return trim_start_; }
  std::uint32_t trim_floor() const noexcept { return floor_; }  // OS-SREC-LEVEL: the leading frames of a level take that are no audio (even; 0 for any other take): the start marker cannot go below
  std::uint32_t trim_end() const noexcept { return trim_end_; }
  std::uint32_t capacity() const noexcept { return capacity_; }
  const std::int16_t* take_data() const noexcept { return take_; }
  const TakeOverview& overview() const noexcept { return ov_; }
  // The overview as a PeakCache over its own sample sequence (the wave view draws from it; WaveWindow{0, overview().samples()} is the whole take). Rebuilt by poll() whenever the
  // overview moved; valid() is false while the take is empty.
  const ui::plot::PeakCache& view_cache() const noexcept { return view_; }
  hal::capture::Source source() const noexcept { return mode_source(cfg, mode_); }
  std::uint8_t gain() const noexcept { return gain_of(source()); }
  bool has_gain() const noexcept { return source_info(source()).gain_bank != kNoGain; }
  const hal::capture::RadioStatus& radio() const noexcept { return radio_; }
  // The meter channel (audio::meter_ch) of the left side of what is being recorded (the right one follows).
  unsigned meter_channel() const noexcept;
  // The seconds of audio in `frames` at the engine's rate, in tenths (the screen's clock).
  static std::uint32_t tenths(std::uint32_t frames) noexcept;
  std::uint32_t generation() const noexcept { return generation_; }  // moves whenever anything above changed: the screen redraws

 private:
  std::uint8_t gain_of(hal::capture::Source s) const noexcept {
    const std::uint8_t bank = source_info(s).gain_bank;
    return bank == kNoGain ? 0u : cfg.gain[bank];
  }
  void touch() noexcept { ++generation_; }
  void apply(std::uint64_t now_ms, bool playing) noexcept;
  bool push_plan(const EnginePlan& want) noexcept;
  bool push_param(core::ParamId id, float value, std::uint32_t ramp) noexcept;
  void finish() noexcept;
  void give_up_take() noexcept;
  // OS-RECCOUNT: arm() with an optional count-in phase (count_in != 0: the tap is armed later, by count_in_poll, during the last count-in bar).
  ArmResult arm_impl(bool playing, std::uint32_t tempo_x100, unsigned count_in, unsigned step, unsigned pattern, bool from_stop) noexcept;
  // OS-RECCOUNT: the count-in phase is over without a take (a Stop or F1 during it, or the arena cleared under it): nothing was recorded, the tap was never armed.
  void give_up_count_in(bool lost) noexcept;
  // SampleBank::clear() (a project load, NEW, a kit) is about to empty the arena: a loop that reads it is released first, so that the render lets go before a loader overwrites it (a fade of one block).
  static void on_bank_clear(void* self) noexcept;
  static bool bank_clear_busy(void* self) noexcept;  // the render has not yet let go of the loop, or still has a take armed or recording, or still holds the DJ scratch ring (OS-DJ C1: the arena's other owner; the File actions refuse a load while it runs)

  Wiring w_;
  State state_ = State::Idle;
  Note note_ = Note::None;
  RecMode mode_ = RecMode::Sources;
  bool in_view_ = false;
  bool playing_ = false;         // the transport runs, as the last poll saw it (OS-REC2: whether STOP AT has a timeline)
  bool stop_pending_ = false;    // a stop at a boundary was asked for and has not come
  bool auto_end_ = false;        // the take in progress ends by itself (RecStop::Bars)
  bool loop_take_ = false;       // OS-LOOPER: the take in progress is a loop's (announced to the loop player, armed bar-aligned with 16 N steps)
  bool loop_review_ = false;     // the Review take is a loop that plays
  bool saved_ = false;           // the Review take was written as a WAV and the window is the one written (note_saved); a new take or a moved marker clears it
  std::uint8_t loop_bars_ = 0;   // the bars of the loop being recorded or under review
  std::uint32_t loop_tempo_ = 0;
  audio::RecTap::Align wait_align_ = audio::RecTap::Align::Free;  // what the armed take waits for (arm(): effective_align); meaningful while Armed
  // OS-RECCOUNT: the count-in phase of the armed take (count_in_bars_ != 0 while it runs: the tap is not armed yet). count_in_seen_ counts the bar boundaries that have come since the
  // phase began (C_0, C_1, ...; the take starts at C_count_in); ci_prev_ / ci_len_ / ci_pattern_ are the position the last count_in_poll() read (kCiNone: none yet), ci_from_stop_ that the
  // transport was stopped when the phase began (its start at step 0 is then C_0). ci_to_boundary_ is the last pass's steps to the next boundary (the arm's last-step guard), ci_arm_tries_
  // the failed arm attempts since the count was reached, ci_arm_failed_ that the phase was given up over them.
  std::uint8_t count_in_bars_ = 0;
  std::uint8_t count_in_seen_ = 0;
  std::uint32_t ci_prev_ = kCiNone;
  std::uint32_t ci_len_ = 0;
  std::uint8_t ci_pattern_ = 0;
  unsigned ci_to_boundary_ = 0xFFFFu;
  unsigned ci_arm_tries_ = 0;
  bool ci_arm_failed_ = false;
  bool ci_from_stop_ = false;
  static constexpr std::uint32_t kCiNone = 0xFFFFFFFFu;
  static constexpr unsigned kCiArmMaxTries = 128;  // passes (about 2 s) the phase retries a refused arm before it gives the count-in up
  bool ov_wanted_ = false;       // OS-LOOPER L2c: overdub-on was posted and not ended (the session may not have begun in the render yet)
  bool ov_seen_ = false;         // overdubbing() / restoring() as the last poll saw them (a change redraws, applies the plan, and rebuilds the overview when it ends)
  bool rst_seen_ = false;
  bool ov_refill_ = false;       // the overview is being fed again after a session or a restore changed the loop
  std::uint64_t save_t0_ = 0;    // now_ms_ at begin_save()
  bool save_wait_ = false;       // SaveWait: quiesce was posted for a SAVE and is not yet acknowledged
  std::uint32_t ov_seq_ = 0;     // the last overdub command posted (LoopPlayer::acked)
  std::uint32_t save_seq_ = 0;
  std::uint32_t undo_seq_ = 0;
  RecMode loop_mode_ = RecMode::Sources;  // the page the loop was recorded on: while it exists the engine's input plan stays on it (Q9: the overdub hears the source the loop was made from)
  bool owned_ = false;           // the recorder owns the engine's input path
  bool settling_ = false;
  bool dirty_ = true;            // the config changed since the hardware and the engine were told
  bool plan_known_ = false;      // `sent_` is what the engine has (it is the board's before the recorder first owns the path)
  EnginePlan sent_;
  bool hw_valid_ = false;
  hal::capture::Source hw_source_ = hal::capture::Source::LineMix;
  std::uint8_t hw_gain_ = 0;
  std::uint64_t settle_until_ = 0;
  std::uint64_t hw_retry_at_ = 0;
  std::uint64_t now_ms_ = 0;
  std::uint32_t radio_generation_ = 0;
  hal::capture::RadioStatus radio_;
  std::int16_t* take_ = nullptr;
  std::uint32_t capacity_ = 0;
  std::uint32_t frames_ = 0;
  std::uint32_t shown_tenths_ = 0;
  std::uint32_t trim_start_ = 0;
  std::uint32_t floor_ = 0;  // OS-SREC-LEVEL: RecTap::settle_ring()'s answer (main loop only); set_trim_start() clamps to it, finish() starts the marker on it, crop()/give_up_take() clear it
  std::uint32_t trim_end_ = 0;
  std::uint32_t generation_ = 0;
  TakeOverview ov_;
  static constexpr std::size_t kViewElems = ui::plot::PeakCache::elements_required(2u * TakeOverview::kSlots, 1);
  ui::plot::PeakCache view_;
  alignas(8) std::int16_t view_store_[(kViewElems + 3u) & ~std::size_t{3}] = {};
  std::uint32_t view_epoch_ = 0xFFFFFFFFu;
  void rebuild_view() noexcept;
};

// The Record screen's page as the recorder's mode: SOURCES, RESAMPLE, RADIO (product-ux section 4: "pages RESAMPLE, RADIO").
constexpr RecMode rec_mode_of_page(unsigned page) noexcept { return page == 1u ? RecMode::Resample : (page == 2u ? RecMode::Radio : RecMode::Sources); }

}  // namespace o3os::app
