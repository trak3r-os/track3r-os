// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Sequencer v2 (OS-005c; ADR 0013 D5 "C. block-synchronous inside the render, sample-exact offsets"): a pure function
//
//     step(block_start, n) -> events[]            over a uint64 sample timeline
//
// with no ISR, no second clock, no race with the voices (hazards H1/H2/H4/H12 of a design with an interrupt-driven sequencer are gone by construction), deterministic and
// therefore testable on a PC and renderable offline faster than real time. The render calls step() once per block and hands the events to the voices
// (OS-005d), to the MIDI out stage (midi/out.h) and to the UI.
//
// TIME. The grid is the file format's: 192 PPQN, 48 microsteps to a step, 4 steps to a beat (12
// nanosteps to a microstep, 576 to a step). A tempo is samples per microstep in Q32.32 (core/qtime.h): exact rational arithmetic, no drift. Microstep j of a
// step that starts at S and lasts D is at S + D j / 48, rounded to the nearest sample for the event, so the microsteps of a step are as even as the
// sample grid allows and a step's length is exact. Event offsets are 0..n-1; an event whose time has already passed (a command that lands mid-block) comes out at 0.
//
// SWING (step-pair arithmetic, recorded as test vectors in tests/data/seq_oracle_vectors.h). With s the swing (25..75, 50 = straight) and
// nominal = 15 s / BPM, step n of the song lasts
//        nominal * (150 - s) / 100        for even n
//        nominal * ( 50 + s) / 100        for odd n
// so the pair always adds to two nominal steps, and every microstep of a step is equally spaced inside it. The pair parity starts at even at every stop. The
// ratios are exact integers over Q32.32 (576 nanosteps to a step).
// PAR-081's Swing FX (17, 25..75) sets s from the row it is in until another one or the stop ("changes pattern swing"); with an external clock s is 50.
//.
//
// TEMPO. The tempo in force is the performance override if there is one (the Tempo FX 15, value * 2 BPM, PAR-082:
// below 40 BPM it is 40; the Performance layer's own override), else the project tempo (smoothed: the sequencer's own ParamChange, below). The Tempo FX takes
// effect from the row it is in: the tempo is read after the row's FX have run, so the step that holds the FX already uses it.
// Tempo fine (core FX 44) adds (v - 128) / 4 BPM to the tempo of its row (PAR-082-like precision for MED imports, med.md section 4), and Speed (core FX 43) sets
// the ticks per step (1..32, default 6) that the Tick events divide a step into; it does not change the step's length.
//
// THE CLOCK. MIDI clock out is generated on the NOMINAL grid (24 ticks per beat, 8 microsteps, spaced by the tempo and not by the swing): sending it every 8th
// microstep would make it uneven under swing; a clock must be steady, and "clock is sacred" (radar card). Tempo changes move it from the
// row that makes them. The events carry the fractional sample, so that the MIDI out stage places each tick to a fraction of a sample.
//
// HALF / DOUBLE TIME (OS-PERF2, new Community OS behaviour; Perform F4 / F5, momentary). A rate of x0.5 or x2 applies to the step rate, on top of whatever tempo is in force (the project's,
// the Tempo FX, the performance override), and changes where it is read: at the first microstep of a step, so a change lands on a step boundary and the song position does not move.
// Internal clock: the step's length (and the MIDI clock out and the Tick events with it) is 2x or 1/2 of the tempo's. External clock (slave): a step spans 12 or 3 of the master's clocks
// instead of 6, so the song runs at half or double the master's tempo and keeps its phase; the master's clock is relayed at what plays. Swing keeps its ratios (a pair is still two
// nominal steps). Samples are not touched: only the step grid moves, so they keep their pitch. tempo_x100() reports what plays (the voices' tempo-synced LFOs follow the steps),
// nominal_tempo_x100() the tempo before the rate.
//
// ROWS, FX, NOTES. At the first microstep of a step each track reads its row (the generation-checked copy, seq/rows.h), sanitises the FX (seq/fx.h) and runs
// the pre-effects in a fixed order (second column, then first); the note starts at that microstep or, with Micro-move, at the
// microstep the FX asks for (seq/fx.h micromove_start). From there the microstep counters rule, in microsteps: a started step stays
// open, its timer counting; a note ends when its timer reaches its length (9999 microsteps unless a Roll or a Gate shortens it); a Roll repeats the note every
// period (the first repeat one microstep late) and its note is half a period long; Roll directions change the
// note or the velocity of the repeats. Where this sequencer differs from what the release notes or the official manual describe, the difference is listed in the spec (open items; not verified).
//
// CHORD, ARP, TUNE (OS-PFX; docs/specs/OS-PFX.md). When a note starts, Tune 40 shifts it, Chord 14 adds the notes of a chord (a MIDI instrument sends them all on the note's sample; any other
// plays the root alone) and Arp 21 plays that chord note by note, exactly `period x n` microsteps after the note's start, on any instrument (pfx_note, arp_next). The per-note state
// is PfxState, apart from TrackState, which every track walks on every microstep. Slide 41 / 42 stay voice FX (control layers below): the sampler voice ramps its pitch (voices.h SLIDE FX).
//
// CONTROL LAYERS. The FX that drive a voice rather than the sequencer go through seq/control.h, which arbitrates the first column, the second column and the
// performance layer per parameter group, and the sequencer emits ModSet / ModEnd events only when a group's winner changes.
//
// PLAYHEADS AND SWITCHING (PAR-021). The song cursor (order position, pattern, step) runs the pattern; every track has its own cursor and its own length (polymetric
// tracks wrap by themselves and all restart together when the pattern ends, which is when the cursor reaches the longest track's end, or a Break pattern / Order
// jump asks, or the performance layer's pattern length does). At the end of a pattern the next one is, in order of precedence: the one queued with queue_pattern()
// (the sequential switch: then the mode becomes Pattern; whether the official firmware has such a switch, and on which keys, is an open research question), the next position of the order list (Song mode; the list loops, or stops, see
// set_song_loop), or the same pattern (Pattern mode: the pattern loops, the transport does not stop). A mode switch requested with request_mode() takes effect at
// the pattern's end as well. Nothing stops, nothing is cut: the next step simply starts on the beat.
// SWITCHING AT ONCE (OS-PATSWITCH, new Community OS behaviour; docs/specs/OS-PATSWITCH.md): switch_pattern(id) does not wait for the pattern's end. At the next step boundary the playing pattern becomes
// `id`, the mode becomes Pattern (a song is left) and every track goes on at the same row: the next row of the old pattern (0 when the old pattern ends at that boundary) modulo the new pattern's
// running length. The grid, the swing pair and the Step events are untouched (the row changes, the step count does not). A pattern the project lacks plays as empty at the default length, as
// at a queued switch. The old pattern's OFF rows are skipped, so on the first step of the new pattern each track ends what it still sounds (switch_release): MIDI notes get their off, a sampler
// note its release (not a cut) unless the new row starts a note of its own. A whole-pattern paste over the pattern that plays skips the OFF rows of the old content in the same way, so release_pattern()
// ends the same at the next step boundary, on the tracks that play that pattern (OS-BUG-PASTEHANG).
//
// PERFORMANCE LAYER (BASE-20; seq/control.h). Per track a source pattern that replaces the song's for that track (switched at the track's own loop end), a length, a play mode (forward, backward, ping-pong, random) and a stutter period (the performance stutter); globally a tempo
// override and a pattern length; and the performance values of the control table. The Perform screen (P2) is their only user; the commands are here, pure and tested.
//
// TRANSPORT. play_pattern(), play_song(), stop(), resume() (continues from where stop() left the song cursor, at the start of that step), locate() (the step number of
// an SPP), plus the external transport events (apply(TransportEvent)). Stop sends every open note off, the MIDI stop and cancels what was scheduled. Here stop()
// remembers the step it was in, which only resume() and a Continue use.
//
// EXTERNAL CLOCK. set_follow() hands the microstep timeline to a midi::ClockDll: microstep m after the transport's first clock is the loop's time for clock m/8 plus
// m % 8 eighths of the way to the next, shifted by the port's user offset (a delay is a shift; an advance is limited to one clock period, because the loop only
// predicts one clock ahead). Swing is off. The loop never revises a time it has given, so a microstep once placed is final (midi/clock_dll.h). A start of our own under an
// external clock (the Play key, a Continue from the UI) begins on the master's next clock, quantised to its grid (next_follow_clock); the master's own Start and Continue
// begin on the clock MidiIn names (OS-005c2).
//
// CONTEXTS. Everything but commands() runs in the render context (one thread of control: step() and every method below are called by it, never concurrently).
// The main loop talks to the sequencer through the command ring (SPSC, core/ring.h): commands() is its producer side; step() drains it at block start. ParamChanges for the
// sequencer (tempo, swing) arrive through the engine's ParamChange drain, which offers each one to apply().
//
// REAL TIME. step() does bounded work: the microsteps that fall inside the block (at most n / 6 + 1: the fastest microstep, 800 BPM at double time with the widest swing, is
// 6.5 samples) for up to 16 tracks each, no allocation, no I/O, no double, no loops over the pattern except the roll-length look-ahead (at most 128 reads, and only
// for a volume roll). The state is about 8 KiB (the pending-event queue is 6 KiB of it); it lives in the render's RAM, not in the model arena.

#include <o3os/audio/events.h>
#include <o3os/audio/play_position.h>
#include <o3os/audio/smoothing.h>
#include <o3os/core/param.h>
#include <o3os/core/qtime.h>
#include <o3os/core/ring.h>
#include <o3os/midi/clock_dll.h>
#include <o3os/midi/in.h>
#include <o3os/midi/timed_queue.h>
#include <o3os/model/project.h>
#include <o3os/seq/control.h>
#include <o3os/seq/fx.h>
#include <o3os/seq/midi_route.h>
#include <o3os/seq/rng.h>

#include <atomic>
#include <cstdint>

namespace o3os::seq {

inline constexpr std::uint32_t kMicrostepsPerStep = 48;
inline constexpr std::uint32_t kPpqn = 192;
inline constexpr std::uint32_t kMicrostepsPerClock = 8;         // 24 PPQN
inline constexpr std::uint32_t kStepOpenLimit = 9999;           // a step stays open this many microsteps
inline constexpr std::uint32_t kDefaultTicksPerStep = 6;        // core FX 43's default (med.md section 4)
inline constexpr std::uint8_t kMinTempoFxBpm = 40;              // PAR-082: "values below 40 treated as 40"
// Our choice (spec 005c): each FX column acts alone, so a sequencer FX consumed in the second column does not stop the voice FX of a step without a note in the first. Setting this constant makes the second column stop the first.
inline constexpr bool kSkipVoiceFxAfterSeqFx = false;

// The length of one step in Q32 samples: kMicrostepsPerStep microsteps of `spt` (samples per tick, Q32, core::samples_per_tick_q32 at the
// tempo in force and the step rate), scaled by the swing pair's factor for this step's parity (even: 150 - s, odd: 50 + s, percent; a pair
// always adds to two nominal steps, see SWING above). One function for finish_step and the song time map (song_time.h, OS-MTC A1), so the
// two cannot drift.
constexpr core::Q32 step_length_q32(core::Q32 spt, std::uint32_t swing_percent, bool swing_odd) noexcept {
  const core::Q32 nominal = static_cast<core::Q32>(kMicrostepsPerStep) * spt;
  const std::uint32_t factor = swing_odd ? 50u + swing_percent : 150u - swing_percent;
  return nominal * factor / 100u;
}

// The boundary a deferred mute toggle waits for (OS-MUTE): the bar (16 steps of the pattern, counted from its start) or the pattern's end (= the next pattern start).
enum class MuteQuant : std::uint8_t { Bar = 1, Pattern = 2 };
inline constexpr std::uint32_t kStepsPerBar = 16;

enum class PlayMode : std::uint8_t { Pattern = 0, Song = 1 };
enum class TrackPlayMode : std::uint8_t { Forward = 0, Backward = 1, PingPong = 2, Random = 3 };  // the play modes of this OS (the official manual's custom orders are not built, official-manual-map)

// The sequencer's own ParamChange ids: kind Project, engine fields from 0x200 (the model's tables stop at 0x100; core/param.h). Values in the units shown.
namespace seq_param {
inline constexpr unsigned Tempo = 0x200;  // BPM 40..800 (a float: 120.5), smoothed by the ParamChange's ramp
inline constexpr unsigned Swing = 0x201;  // 25..75
inline constexpr unsigned Metro = 0x202;  // OS-METRO: 0 or 1, whether the sequencer emits the Metro events (the metronome is on); the count-in is not affected
inline constexpr unsigned MidiProgram = 0x203;  // OS-INSTUI S8b: the value is the instrument slot 0..63 whose PROGRAM was edited: the program is sent now (on its channel) if the instrument is a MIDI one that has a program
}  // namespace seq_param
constexpr core::ParamId tempo_param() noexcept { return core::param_id(core::ParamKind::Project, 0, seq_param::Tempo); }
constexpr core::ParamId swing_param() noexcept { return core::param_id(core::ParamKind::Project, 0, seq_param::Swing); }

// The main loop's commands (the SPSC ring behind Sequencer::commands()).
struct Command {
  enum class Kind : std::uint8_t {
    PlayPattern,    // a pattern id, b count-in bars 0..2 (OS-METRO), c from step
    PlaySong,       // b order position, c from step
    Stop,
    Resume,
    Locate,         // c step
    LocateRow,      // c row of the pattern now playing (OS-PLAYCUR): the line jumps there at the next step boundary
    QueuePattern,   // a pattern id
    SetMode,        // a PlayMode, at the pattern end
    Select,         // a PlayMode, b pattern id: what an external Start plays (stopped only)
    PerfTrack,      // a track, b pattern id (0: follow the song), c length (0: the pattern's) | mode << 8
    PerfStutter,    // a track, b value 0..15
    PerfTempo,      // c BPM x 100, 0 none
    PerfPatternLen, // c steps, 0 none
    PerfControl,    // a track, b group, c value (int16), flags bit 0: end
    MuteQuant,      // a track, b boundary (MuteQuant::Bar / Pattern), c the track's effective mute until then (0 or 1): the project's flag is already the new one
    MuteCancel,     // a track: the deferred toggle is cancelled (the second press)
    QueueJump,      // b order position (kNoJump: cancel), c the stamp (Sequencer::jump_stamp(); 0: none): the song goes there at the end of the current pattern (OS-SONG1)
    PerfRate,       // a 0 normal, 1 half time, 2 double time (OS-PERF2): the step rate, applied at the next step boundary
    SwitchPattern,  // a pattern id 1..255 (OS-PATSWITCH): the playing pattern becomes it at the next step boundary, on the same row; the pattern already playing withdraws a switch that waits
    ReleasePattern, // a pattern id 1..255 (OS-BUG-PASTEHANG): the pattern was rewritten under the run; at the next step boundary the tracks that play it end what they still sound (switch_release)
    QueueTrackPattern,  // b pattern id 0..255 (0: back to the song's), c mask of tracks (bit t: track t) (E-PERF-07): see Sequencer::queue_track_pattern
  };
  Kind kind;
  std::uint8_t a;
  std::uint16_t b;
  std::uint32_t c;
};
static_assert(sizeof(Command) == 8, "a command is two words");
inline constexpr std::uint16_t kNoJump = audio::PlayPosition::kNoOrder;  // "no jump queued"
using CommandRing = core::SpscRing<Command, 32>;

struct Position {
  bool playing;
  PlayMode mode;
  std::uint16_t order_pos;     // the song cursor's position in the order list (0xFFFF in Pattern mode)
  std::uint8_t pattern_id;
  std::uint16_t step;          // the song cursor's step in the pattern
  std::uint16_t pattern_len;   // steps
  std::uint32_t tempo_x100;    // in force
  std::uint8_t ticks_per_step;
  std::uint8_t ustep;          // the microstep of that step the playhead is in (0..47: the last one run); OS-LIVEREC: the UI rounds a live note to the nearest step by it
  std::uint8_t count_in;       // OS-METRO: the steps of the count-in still to run, the one running included (0: not counting in; at most 32)
};

// E-SONG-07: the render's copy of the song list (`cur`) and the scratch an edit is read into (`next`), 656 B. On the target (O3OS_SEQ_ORDER_EXTERNAL) the store is the caller's, in SDRAM:
// SRAM_U has no room for it (the 32 KiB free floor of ADR 0013 D7), and the list is read once a pattern end and once an edit, as the render already reads the project from SDRAM. A sequencer
// without a store plays an empty song. On the host the store is inside the sequencer.
struct OrderStore {
  model::OrderSnapshot cur;
  model::OrderSnapshot next;
};

class Sequencer {
 public:
  static constexpr std::uint32_t kMaxTracks = model::kMaxTracks;
  static constexpr std::uint32_t kQueueSize = 256;

  struct Config {
    bool song_loop = true;          // at the end of the order list: false stops the transport (what the official firmware does at the song end is an open research question)
    bool emit_ticks = false;        // Tick events for Speed (an Amiga-engine instrument turns it on)
    bool emit_clock = false;        // MIDI clock, start, continue, stop and SPP events
    bool emit_transport = true;     // E-CFG-03 (TRANSPORT OUT): with emit_clock on, false sends the clock but no Start, Continue, Stop or SPP. Read when a run begins (run_transport_), so a switch mid-run keeps the run's Stop
    std::uint32_t seed = 1;         // the random generator's seed, applied at every transport start
    std::uint32_t start_lead = 0;   // samples between a play() and the first step; with MIDI out on, a block gives SPP and the start byte room to go first
    bool pattern_once = false;      // Pattern mode: the transport stops at the end of the pattern instead of looping it (the export's: OS-WAVEXP; a Song's end is song_loop)
    std::uint16_t pattern_stop = 0; // with pattern_once: the transport stops at the start of this step of the pattern (the export of a selection: the row after its last one); 0: at the pattern's end
    std::uint16_t order_stop = 0;   // Song mode: the transport stops where the song would go on to this order position or past it (the export of a range of slots: the position after the last one; OS-SONGSHARE S5); 0: at the end of the list
  };

  struct Stats {
    std::uint32_t microsteps = 0;
    std::uint32_t steps = 0;
    std::uint32_t events = 0;
    std::uint32_t late_events = 0;      // emitted at offset 0 because their time had passed
    std::uint32_t deferred = 0;         // held to the next block because the output array was full
    std::uint32_t queue_overflow = 0;   // lost: the pending queue was full
    std::uint32_t rows_torn = 0;        // rows that could not be copied consistently (seq/rows.h)
    std::uint32_t rows_missing = 0;     // rows of a pattern that does not exist (it plays as empty)
    std::uint32_t microstep_overrun = 0;// blocks whose microstep guard ran out (a fault: the rest ran next block, late)
    std::uint32_t commands = 0;
    std::uint32_t fx_ignored = 0;       // FX that were inert or ignored by a rule (Order jump out of range, ...)
    std::uint32_t queue_dropped = 0;    // E-SONG-07: the queued jumps that a delete of their slot or an overflow of the edit journal cancelled
  };

  using OrderStoreType = OrderStore;
  explicit Sequencer(const model::Project& project) noexcept;
  Sequencer(const Sequencer&) = delete;
  Sequencer& operator=(const Sequencer&) = delete;

  // E-SONG-07: gives the sequencer its list store (zeroed here), before the first step(). Needed on the target only; the host's is inside.
  void attach_order_store(OrderStore& store) noexcept;

  void configure(const Config& c) noexcept;
  const Config& config() const noexcept { return cfg_; }

  // ---- the main loop's side --------------------------------------------------------------------------------------------
  CommandRing& commands() noexcept { return cmds_; }

  // ---- the render's side -----------------------------------------------------------------------------------------------
  // The project the sequencer plays (read-only): MidiLink::live_event looks up the kind and channel of the instrument a pad plays (OS-INSTUI S2b), as send_note_on does.
  const model::Project& project() const noexcept { return project_; }
  // One block: the events of [block_start, block_start + n) into `out` (at most `cap`, sorted by offset), returns how many. n >= 1.
  std::uint32_t step(std::uint64_t block_start, std::uint32_t n, audio::SeqEvent* out, std::uint32_t cap) noexcept;

  // A ParamChange of the sequencer's (seq_param): true when the id was one of them.
  bool apply(const core::ParamChange& pc) noexcept;

  // OS-INSTUI S8b (render context): the MIDI side's memory of what the receivers were told. The program change that must go out before a note of the MIDI instrument `in`, or -1: its PROGRAM (when it has
  // one) differs from what was last sent on its channel, or nothing was sent since the project was loaded (load_epoch). The caller sends it and then calls program_sent(): a step's program that found no room in
  // the event queue is not recorded as told (the pad path cannot know: MidiOut::send does not report a full staging queue, so a pad's program is taken as told once it was handed over). The notes of the steps (send_note_on) and of the pads (MidiLink::live_event) both ask, so that one note of either is enough to put the receiver on the program.
  int program_due(const model::Instrument& in) noexcept;
  void program_sent(const model::Instrument& in, int program) noexcept;

  // Transport. They take effect at the start of the next block (`now`, the block start of the next step() call).
  // `count_in_bars` (OS-METRO, 0..2; Pattern mode, a fresh start only; OS-COUNTIN): the transport counts that many bars (16 steps each) on `from_step` and only then plays it. During the count-in the
  // play line WAITS on `from_step` (the position's step stays there), the tracks play nothing, the click sounds if the metronome is on, the MIDI clock runs, and the Step events (one per step, all with
  // `from_step`) carry kEvCountIn (events.h); the position carries count_in > 0. `from_step` is the first step played when the last of them ends. A stop ends it.
  void play_pattern(std::uint32_t pattern_id, std::uint32_t from_step = 0, std::uint32_t count_in_bars = 0) noexcept;
  void play_song(std::uint32_t order_pos = 0, std::uint32_t from_step = 0) noexcept;
  void stop() noexcept;
  void resume() noexcept;
  // The step number (MIDI beats of an SPP) as a position: Song mode walks the order list from its start, Pattern mode wraps by the pattern's length. Takes effect at
  // the next play or resume; while playing it jumps there at the next step.
  void locate(std::uint32_t step_index) noexcept;
  // OS-PLAYCUR: while playing, the play line jumps to `row` (modulo the pattern's length) of the pattern now playing, in Pattern or Song mode, at the next step boundary: the step that is running ends, the
  // row runs next, once (nothing is played twice or skipped on the way). Ignored when stopped and during a count-in (the line waits on its start point there).
  void locate_row(std::uint32_t row) noexcept;
  // OS-PATSWITCH: while playing, the playing pattern becomes `pattern_id` (1..255) at the next step boundary and the line goes on at the same row (see SWITCHING AT ONCE above). The newest intent wins: it
  // drops a queued pattern, a queued live jump, a requested mode and a locate that waits, and a later locate_row() or locate() drops it. The pattern that already plays, in Pattern mode, starts no switch and withdraws one
  // that waits (like every pick it still drops a queued pattern, a queued live jump, a requested mode and a locate that waits); in Song mode it leaves the song and loops that pattern. Ignored when stopped, during a count-in and for an id outside 1..255. A stop, a start and a resume leave nothing waiting.
  void switch_pattern(std::uint32_t pattern_id) noexcept;
  // OS-BUG-PASTEHANG: pattern `pattern_id` (1..255) was rewritten while the transport runs (the whole-pattern paste): at the next step boundary every track that plays it ends what it still sounds, as at a
  // landing (switch_release: a MIDI note gets its off, a sampler note its release, unless the row of that step starts a note of its own). Ignored when stopped and during a count-in; a stop or a start
  // drops it. Two ids before one boundary release every track.
  void release_pattern(std::uint32_t pattern_id) noexcept;
  // The last landing of a switch for the main loop's log, one word the render writes (the one writer): landings so far (16 bits) << 16 | pattern << 8 | row. 0 before the first.
  std::uint32_t last_landing() const noexcept { return landing_pub_.load(std::memory_order_relaxed); }
  void queue_pattern(std::uint32_t pattern_id) noexcept { queued_pattern_ = pattern_id > 255u ? 0u : pattern_id; }
  void request_mode(PlayMode m) noexcept { requested_mode_ = m; mode_request_ = true; }
  // The live jump (OS-SONG1; the Song screen's, and OS-PERF2's NEXT PATTERN is the same call with the next position): while playing, the song goes to order position `order_pos` at the
  // end of the pattern now playing, on the render's timeline like every pattern change (nothing is cut; the Song mode starts there if the transport played one pattern). The latest call
  // replaces the one waiting, kNoJump (or a position the song does not have) cancels it; an Order jump FX of the pattern wins at its end (the jump waits for the next one). Ignored when
  // stopped. The main loop reads what waits through play_position().read().queued.
  // E-SONG-07: `stamp` is jump_stamp() read when the position was authored (0: authored against the list the render has now). The position is remapped through the edits of the song's order
  // list made since (an insert above it shifts it, a delete of its slot drops the jump).
  void queue_jump(std::uint32_t order_pos, std::uint32_t stamp = 0) noexcept;
  // The stamp a QueueJump command carries in `c`: the count of order-list edits the app's position was authored against, plus one (0 is "no stamp").
  std::uint32_t jump_stamp() const noexcept { return project_.order_ops() + 1u; }
  void set_song_loop(bool on) noexcept { cfg_.song_loop = on; }
  bool playing() const noexcept { return playing_; }
  PlayMode mode() const noexcept { return mode_; }

  // Performance layer.
  void set_perf_track(std::uint32_t track, std::uint32_t pattern_id, std::uint32_t length, TrackPlayMode mode) noexcept;
  // E-PERF-07: the tracks of `mask` (bit t: track t, bits past kMaxTracks ignored) read source pattern `pattern_id` (0: the song's) from their own next loop end on, while playing (ignored when stopped).
  // It touches neither perf_len nor perf_mode. The same call again for a track that waits takes the queue back (so does pattern 0 for a track that follows the song). The pattern a track already
  // follows pins it: a landed override at once. A landed override stays until a queue of pattern 0 lands or the run ends (a stop, a restart, a resume); PerfTrack ignores the pattern of such a track.
  // A track that cannot reach its own loop end (Random, or longer than the song's pattern, which load_tracks restarts first) lands at the song pattern's end instead.
  void queue_track_pattern(std::uint32_t mask, std::uint32_t pattern_id) noexcept;
  // (The track's override and its wait go to the screen through play_position().track_pattern(t), once a block.)
  void set_perf_stutter(std::uint32_t track, std::uint32_t value) noexcept;
  // The performance layer's tempo override (BPM x 100, 0 none). It stays until it is cleared (the Perform screen owns it) and wins over the Tempo FX, as the performance
  // layer wins over the sequencer's everywhere. (What a stop does to it on the official firmware is an open research question.)
  void set_perf_tempo(std::uint32_t bpm_x100) noexcept { perf_tempo_x100_ = bpm_x100 == 0u ? 0u : (bpm_x100 < model::kMinTempoX100 ? model::kMinTempoX100 : (bpm_x100 > model::kMaxTempoX100 ? model::kMaxTempoX100 : bpm_x100)); }
  void set_perf_pattern_length(std::uint32_t steps) noexcept { perf_pattern_len_ = steps > 128u ? 128u : steps; }
  // The step rate (OS-PERF2, see HALF / DOUBLE TIME): 0 normal, 1 half time, 2 double time (anything else is normal). Kept across a stop and a start (it is the held key's); read at the next step.
  void set_play_rate(std::uint32_t rate) noexcept { rate_ = rate == 1u ? std::uint8_t{1} : (rate == 2u ? std::uint8_t{2} : std::uint8_t{0}); }
  std::uint32_t play_rate() const noexcept { return rate_; }
  void set_perf_control(std::uint32_t track, FxGroup g, std::int16_t value) noexcept;
  void end_perf_control(std::uint32_t track, FxGroup g) noexcept;

  // QUANTIZED MUTE (OS-MUTE). A mute toggle on a track whose MUTE MODE is Bar or Pattern: the main loop writes the project's flag at once (so the Mixer and the file say what the user did) and sends
  // MuteQuant; the sequencer then holds the track's EFFECTIVE mute (what the graph's strip has, and what its MIDI notes follow) at the old value until the boundary. At the first microstep of the
  // step that starts the bar or the pattern it emits StripMute at that exact sample (the engine ramps the strip: no click) with the project's flag as the value, and the hold ends. MuteCancel
  // (a second press: the main loop has put the flag back) drops the hold; nothing was told to the graph. A transport that is not running, and a stop, apply it at once. SOLO is never deferred.
  // mute_pending() is published for the main loop (one word, the render the one writer): the pad blinks while a bit is set.
  std::uint16_t mute_pending() const noexcept { return pending_pub_.load(std::memory_order_relaxed); }
  // The notes the track has started (the sequencer's, not the live notes of the pads), modulo 256: written only here, read by the main loop, which compares it with its last copy to
  // flash a lamp (app::ActivityWatch). Counted when the step starts, whether or not the track is muted.
  std::uint8_t note_count(std::uint32_t t) const noexcept { return t < kMaxTracks ? activity_[t].load(std::memory_order_relaxed) : std::uint8_t{0}; }

  // MUTE AND SOLO (OS-MUTE, new Community OS behaviour; the mute of an audio track acts on its strip's level, not on note starts). A track's mute and solo are the project's (TrackSlot::flags: the Track parameters Mute and Solo, which
  // the graph's strip acts on: the level goes to 0, the voices go on), so a muted SAMPLE, module or Amiga track keeps starting notes and running FX and an unmute is heard at once, mid-note. The graph
  // does not carry MIDI, so for a track whose step plays a MIDI instrument the sequencer honours them itself, at note level: no MidiNoteOn goes out while the track is muted or silenced by
  // another track's solo, and a MIDI note that is sounding gets its off (PAR-063). The flags are read once a block (refresh_silence), so a change lands on the next block's first microstep.

  // External clock. `dll` null (or clear_follow) returns to the internal timeline. `offset_us` is the port's user offset in microseconds (delay > 0).
  void set_follow(const midi::ClockDll* dll, std::int32_t offset_us) noexcept;
  void clear_follow() noexcept { follow_ = nullptr; }
  bool following() const noexcept { return follow_ != nullptr; }
  // What a transport that starts without a command of ours plays (an external Start, a Continue): the mode and, in Pattern mode, the pattern. The UI calls it
  // whenever the edited pattern or the Play Pattern / Play Song choice changes while stopped; a play_pattern / play_song sets it too. In Song mode the pattern is not
  // taken (the song names its patterns: a Start runs it from position 0, and so does a Continue before anything has run, see the resume in step()).
  void select(PlayMode mode, std::uint32_t pattern_id) noexcept {
    if (!playing_) {
      mode_ = mode;
      if (mode == PlayMode::Pattern) {
        pattern_id_ = pattern_id > 255u ? 0u : pattern_id;
      }
    }
  }
  // A transport event from MidiIn: Start (from the beginning, on the clock it names), Continue (from the stopped or located position), Stop, Spp.
  void apply(const midi::TransportEvent& ev) noexcept;

  // ---- reading ---------------------------------------------------------------------------------------------------------
  Position position() const noexcept;
  // The position as the UI reads it (OS-009b): published at the end of every step() call, read by the main loop (audio/play_position.h).
  const audio::PlayPosition& play_position() const noexcept { return published_; }
  std::uint32_t tempo_x100() const noexcept;       // what plays: nominal_tempo_x100() at the step rate (half / double time)
  std::uint32_t nominal_tempo_x100() const noexcept;  // in force before the rate: the performance override, the FX tempo, the smoothed project tempo (or the master's clock)
  std::uint32_t swing() const noexcept;            // in force
  const ControlTable& control() const noexcept { return ctl_; }
  const Stats& stats() const noexcept { return stats_; }
  Rng& rng() noexcept { return rng_; }

  struct TrackState {
    // cursor
    std::uint8_t pos = 0;           // 0-based step in its pattern
    bool back = false;              // ping-pong: going down
    std::uint8_t len = 1;           // its length in steps, as of the pattern it plays
    // the row, as copied at the row's first microstep
    model::Step row = model::kEmptyStep;
    bool cancel = false;            // Chance said no
    bool delayed = false;           // Micro-move
    std::uint8_t start_at = 1;      // microstep (1-based) at which the note starts
    std::uint8_t vel = audio::kVelocityDefault;  // 0..127 or default
    std::uint8_t gate = 0;          // Gate FX of the row (0: none)
    bool force_roll = false;        // a Roll on a step without a note: repeat at once (one microstep)
    // the microstep counters
    bool step_open = false;
    bool note_open = false;
    std::uint16_t step_timer = 0;
    std::uint16_t note_timer = 0;
    std::uint16_t note_len = 0;
    // roll
    bool roll_on = false;
    std::uint8_t roll_col = 0;
    std::uint8_t roll_val = 0;
    std::uint8_t roll_start_vel = 0;
    std::uint16_t roll_len_steps = 1;
    // the note last sent: repeats, offs, and the bookkeeping of a MIDI note
    bool sent_valid = false;
    bool sent_midi = false;
    bool midi_on = false;           // a MIDI note is sounding (an off is owed)
    std::uint8_t sent_note = 0;     // 0..127
    std::uint8_t sent_slot = 0;     // instrument slot 0..63
    std::uint8_t sent_vel = audio::kVelocityDefault;
    std::uint8_t sent_ch = 0;       // MIDI channel 0..15
    std::uint8_t last_slot = 0xFF;  // the last instrument this track played (a step without one repeats it)
    // OS-PFX: an arpeggio runs (the rest of it is PfxState): its note n starts arp_period x n microsteps after the first, while the step is open
    bool arp_on = false;
    std::uint16_t arp_period = 0;   // microsteps between two notes (6 .. 384)
    std::uint8_t trk_cols = 0;      // OS-COREFX: bit c: FX column c held a tracker FX (46..50) on the previous row, so the next row that holds none has to end it (a TrkFx of type 0)
    // performance layer
    std::uint8_t perf_pattern = 0;  // 0: the song's pattern
    std::uint8_t perf_pending = 0;  // a source pattern to switch to at the track's own loop end
    bool perf_pending_set = false;
    bool perf_q = false;            // E-PERF-07: perf_pattern / perf_pending belong to QueueTrackPattern (PerfTrack leaves them), until the run ends
    std::uint8_t perf_len = 0;      // 0: the pattern's track length
    TrackPlayMode perf_mode = TrackPlayMode::Forward;
    std::uint8_t perf_stutter = 0;
    std::uint8_t pattern_id = 0;    // the pattern this track reads now
    std::uint8_t sent_ports = 0;    // OS-INSTUI S8b: the route of the MIDI note last sent (audio::kRoutePorts bits, the delay in ms): its off goes the same way, whatever the instrument is by then
    std::uint8_t sent_delay_ms = 0;
  };

  // OS-PFX: what Chord 14 and Arp 21 make of the note that started, and the chord's MIDI notes that sound. Apart from TrackState, which every track walks on every microstep: this is
  // read when a note starts, when an arp note is due and when a MIDI note ends.
  struct PfxState {
    std::uint8_t chord[kChordMaxExtra] = {};  // the Chord FX's offsets above the note, chord_n of them; none while an Arp plays the chord note by note
    std::uint8_t chord_n = 0;
    std::uint8_t arp_dir = 0;       // ArpDir
    std::uint8_t arp_pos = 0;       // 0..11: where the next search of the arp walk starts (Up and Random 0 at the start, Down 11); arp_pick
    std::uint8_t arp_base = 0;      // the step's note, 0..127
    std::uint16_t arp_mask = 0;     // the semitone offsets above arp_base that the arp visits (bit b, 0..11: base + b, clamped at 127), bit 0 always
    std::uint8_t sent_extra[kChordMaxExtra] = {};     // the chord's other MIDI notes that sound (their offs are owed), sent_extra_n of them
    std::uint8_t sent_extra_n = 0;
  };

  // transport and cursors
  void drain_commands() noexcept;
  // E-SONG-07: the song list the render plays is its own copy (order_), taken from the Project at the start of a block when an edit finished; the song positions it keeps are remapped through the
  // edits' journal. Render context only. Not remapped: positions the app holds on its side (the DJ transition's `from`/`last`/`hold`/`jump`, app/src/dj_actions.cpp): the app ends a transition when
  // the playing order differs from the slot it held (the `play.order != t.hold` race check), so an edit of the list during a transition costs that transition, never a wrong jump; a jump the
  // app queued carries its stamp and is remapped here.
  void adopt_order() noexcept;
  void remap_queue(const model::OrderSnapshot& journal, std::uint32_t from) noexcept;
  std::uint32_t song_len() const noexcept { return store_ != nullptr ? store_->cur.len : 0u; }
  std::uint32_t song_at(std::uint32_t pos) const noexcept { return store_ != nullptr && pos < store_->cur.len ? store_->cur.list[pos] : 0u; }
  void begin_run(PlayMode mode, std::uint32_t order_pos, std::uint32_t pattern_id, std::uint32_t from_step, bool resuming) noexcept;
  void end_run(bool emit_stop, std::uint64_t at) noexcept;
  void reset_tracks() noexcept;
  void load_tracks(std::uint32_t from_step, bool loop = false) noexcept;
  void land_track_queue(std::uint32_t t, std::uint64_t at) noexcept;  // E-PERF-07: track t's queued source pattern becomes its own (the caller sets the track's pattern, length and position)
  std::uint32_t song_pattern_len(std::uint32_t pattern_id) const noexcept;
  std::uint32_t effective_len() const noexcept;
  std::uint32_t song_step_index() const noexcept;
  void advance_tracks() noexcept;
  void pattern_end() noexcept;
  void cancel_track_fx(std::uint32_t t, std::uint64_t at) noexcept;

  // time
  void run_microstep(std::uint64_t at) noexcept;
  void begin_step(std::uint64_t at) noexcept;
  void finish_step() noexcept;
  void end_step() noexcept;
  std::uint64_t micro_time_raw(std::uint64_t m) const noexcept;
  core::Q32 played_spt() const noexcept;            // samples per tick at the nominal tempo and the step rate
  static constexpr std::uint32_t micro_per_clock(std::uint8_t rate) noexcept { return rate == 1u ? kMicrostepsPerClock / 2u : (rate == 2u ? kMicrostepsPerClock * 2u : kMicrostepsPerClock); }
  std::uint64_t next_follow_clock() const noexcept;
  std::uint64_t next_micro_time() noexcept;
  std::uint64_t step_boundary_time() const noexcept;
  void rebase() noexcept;
  void fill_clock(core::Q32 until) noexcept;

  // tracks
  void play_microstep(std::uint32_t t, std::uint32_t u, std::uint64_t at) noexcept;
  void read_row(std::uint32_t t) noexcept;
  void switch_release(std::uint32_t t, std::uint64_t at) noexcept;  // OS-PATSWITCH: the first step after a landing, once per track, after its row is read
  void pre_fx(std::uint32_t t, std::uint64_t at) noexcept;
  void start_step(std::uint32_t t, std::uint64_t at) noexcept;
  void note_start_fx(std::uint32_t t, std::uint64_t at) noexcept;
  // OS-PFX, per note and per arp note: kept out of line so that the track loop of every microstep (play_microstep) stays what it was.
  [[gnu::noinline]] std::uint8_t pfx_note(std::uint32_t t, std::uint8_t note, std::uint8_t slot) noexcept;
  [[gnu::noinline]] void arp_next(std::uint32_t t, std::uint64_t at) noexcept;
  std::uint8_t arp_note(PfxState& x) noexcept;
  void apply_columns(std::uint32_t t, std::uint8_t mask, const std::uint8_t type[2], const std::uint8_t value[2], std::uint64_t at) noexcept;
  void roll_repeat(std::uint32_t t, std::uint64_t at) noexcept;
  void setup_roll(std::uint32_t t, std::uint32_t col, std::uint8_t value, std::uint8_t start_velocity) noexcept;
  void send_note_on(std::uint32_t t, std::uint8_t note, std::uint8_t slot, std::uint8_t vel, bool retrigger, std::uint64_t at) noexcept;
  void send_note_off(std::uint32_t t, std::uint64_t at) noexcept;
  void send_voice_off(std::uint32_t t, audio::EventKind kind, std::uint64_t at) noexcept;
  // OS-INSTUI S8b: a MIDI event with the route of `r` (ports in `flags`, the OFFSET in `value`).
  bool emit_midi(std::uint64_t at, audio::EventKind kind, std::uint8_t track, std::uint8_t a, std::uint8_t b, const MidiRoute& r) noexcept;
  void emit_cc(std::uint32_t t, std::uint32_t slot_index, std::uint8_t value, std::uint64_t at) noexcept;
  void midi_sync_state() noexcept;
  void flush_program_edits() noexcept;
  void centre_bends(std::uint64_t at) noexcept;
  void emit_changes(const ModChange* ch, std::uint32_t n, std::uint64_t at) noexcept;
  static void kill_roll(TrackState& k, int col) noexcept {
    if (k.roll_col == col) {
      k.roll_on = false;  // a new FX of a slot kills the one before
    }
  }
  std::uint8_t roll_velocity(const TrackState& k) const noexcept;
  std::uint16_t roll_length_steps(std::uint32_t t) const noexcept;
  std::uint8_t default_velocity(std::uint8_t slot) const noexcept;
  bool is_midi_slot(std::uint8_t slot) const noexcept;
  void refresh_silence() noexcept;
  void mute_defer(std::uint32_t t, std::uint32_t quant, bool held) noexcept;
  void mute_cancel(std::uint32_t t) noexcept;
  void mute_release(std::uint16_t mask, std::uint64_t at) noexcept;
  // True when the track's MIDI notes are held back: muted, or silenced by the solo of another track (as the graph's strip is). Audio tracks are not affected.
  bool midi_silenced(std::uint32_t t) const noexcept { return ((silent_ >> t) & 1u) != 0u; }

  // events
  // false when the queue was full (counted in stats().queue_overflow): the event is lost
  bool emit(std::uint64_t at, audio::EventKind kind, std::uint8_t track, std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t flags,
            std::int32_t value) noexcept;
  std::uint64_t abs_of(core::Q32 q) const noexcept { return origin_ + core::q32_round(q); }
  // When a command that acts at once takes effect: now, inside step(); between two calls, at the start of the next block.
  std::uint64_t command_time() const noexcept { return in_step_ ? now_ : block_end_; }

  const model::Project& project_;
  Config cfg_{};
  CommandRing cmds_;
  midi::TimedQueue<audio::SeqEvent, kQueueSize> queue_;
  Rng rng_;
  ControlTable ctl_;
  // OS-INSTUI S8b (render context only): the program last sent on each channel (0xFF: none since the project was loaded), the routes each channel's pitch bend went to (bend_routes_), the instruments whose PROGRAM was edited
  // (one bit a slot, taken by step()), and the Project::load_epoch the first two belong to.
  std::uint8_t chan_prog_[16] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  std::uint64_t bend_routes_ = 0;     // four bits a channel, one per route label (`ports` 0..3): the routes a Micro-tune bent and nothing has centred since; the centre goes to every one of them
  std::uint32_t prog_edit_[2] = {0, 0};
  std::uint32_t load_epoch_ = 0;
  TrackState tr_[kMaxTracks];
  PfxState pfx_[kMaxTracks];

  // transport
  bool playing_ = false;
  PlayMode mode_ = PlayMode::Pattern;
  bool mode_request_ = false;
  PlayMode requested_mode_ = PlayMode::Pattern;
  std::uint32_t queued_pattern_ = 0;
  std::uint16_t queued_jump_ = kNoJump;  // the live jump waiting for the pattern's end (OS-SONG1)
  // E-SONG-07 (render context): the render's copy of the song list and the Project's edit journal, the scratch the next one is read into (a failed read leaves the copy as it was), the op count
  // queued_jump_ is expressed in, and "the playing slot was deleted": order_pos_ then already names the slot that slid into its place, which plays next (cleared by every pattern end, start and stop).
  OrderStore* store_ = nullptr;
#ifndef O3OS_SEQ_ORDER_EXTERNAL
  OrderStore own_store_ = {};
#endif
  std::uint32_t queued_from_ = 0;
  bool order_have_ = false;
  bool after_gone_ = false;
  bool pending_start_ = false;
  bool pending_resume_ = false;
  bool pending_stop_ = false;
  std::uint32_t pending_order_ = 0;    // Song: the order position; Pattern: the pattern id
  std::uint32_t pending_from_ = 0;
  std::uint8_t pending_count_in_ = 0;  // OS-METRO: the bars play_pattern() asked to count in, taken by the next fresh begin_run()
  PlayMode pending_mode_ = PlayMode::Pattern;
  bool have_resume_ = false;
  bool resume_gone_ = false;  // E-SONG-07: the slot the stopped song was in was deleted (resume_order_ is the one that slid in; a Continue plays it after the rest of that pattern)
  std::uint32_t resume_order_ = 0;
  std::uint32_t resume_step_ = 0;
  std::uint32_t resume_pattern_ = 0;
  PlayMode resume_mode_ = PlayMode::Pattern;
  // OS-PATSWITCH (render context; the command ring is the main loop's way in): its own members, apart from located_ (which a resume reads, begin_run / end_run clear these).
  bool switch_pending_ = false;        // a switch waits for the next step boundary (switch_pattern)
  std::uint8_t switch_id_ = 0;         // ... to this pattern
  bool switch_release_ = false;        // the step after a landing: each track ends what the skipped OFF rows would have (read in play_microstep, cleared by finish_step)
  bool release_req_ = false;           // release_pattern() waits for the next step (run_microstep turns it into switch_release_)
  std::uint8_t release_id_ = 0;        // ... for this pattern (0: every track)
  std::uint8_t release_only_ = 0;      // while switch_release_ runs: only the tracks of this pattern release (0: every track, as after a landing)
  std::atomic<std::uint32_t> landing_pub_{0};  // last_landing(): the render is the one writer, the main loop reads it
  bool located_ = false;
  bool locate_pending_ = false;
  bool locate_in_place_ = false;  // E-SONG-07: the pending locate is a locate_row() (the playing pattern, a new row), not an SPP / locate() to a slot
  std::uint32_t located_order_ = 0;
  std::uint32_t located_step_ = 0;
  std::uint32_t located_pattern_ = 0;
  std::uint64_t now_ = 0;              // the current block's first sample
  std::uint64_t block_end_ = 0;        // its end: the start of the next block, between two calls
  bool in_step_ = false;

  // OS-METRO (render context; the command ring and the ParamChange are the main loop's ways in)
  bool metro_on_ = false;              // seq_param::Metro: emit a Metro event on each beat
  std::uint16_t count_in_total_ = 0;   // steps of the count-in that is running (16 x bars), 0: none
  std::uint16_t count_in_left_ = 0;    // of them, still to run, the step now running included

  // the song cursor
  std::uint32_t order_pos_ = 0;
  std::uint32_t pattern_id_ = 0;
  std::uint32_t pattern_len_ = 1;
  std::uint32_t song_step_ = 0;        // the song cursor's step in the pattern
  bool break_pending_ = false;
  bool jump_pending_ = false;
  std::uint32_t jump_order_ = 0;

  // timeline (internal clock)
  std::uint64_t origin_ = 0;
  core::Q32 step_start_ = 0;
  core::Q32 step_dur_ = 0;
  std::uint32_t ustep_ = 0;            // the next microstep to run, 0..47
  std::uint64_t next_abs_ = 0;         // its sample
  bool swing_odd_ = false;             // false for the first step of a pair
  std::uint64_t micro_index_ = 0;      // microsteps since the run began
  core::Q32 clk_next_ = 0;
  core::Q32 clk_tl_ = 0;
  bool clk_on_ = false;
  bool run_transport_ = true;          // E-CFG-03: Config::emit_transport as it was when the run began (render thread only): the run's Stop follows its Start, whatever the switch is by then

  // tempo
  audio::Smoother tempo_sm_;           // BPM x 100 (the smoother is generic over int32)
  std::uint32_t fx_tempo_x100_ = 0;    // the Tempo FX's override; 0: none (reset by a stop and a start)
  std::uint32_t perf_tempo_x100_ = 0;  // the performance layer's; 0: none
  std::int32_t tempo_fine_row_ = 0;    // x100 BPM added by the row's Tempo-fine FX
  std::uint8_t swing_fx_ = 0;          // 0: none
  std::uint32_t swing_param_ = 0;      // 0: none (the ParamChange)
  std::uint32_t ticks_per_step_ = kDefaultTicksPerStep;
  std::uint32_t perf_pattern_len_ = 0;
  std::uint8_t rate_ = 0;              // the step rate: 0 normal, 1 half time, 2 double time (render context; the command ring is its one writer)
  std::uint8_t micro_per_clock_ = kMicrostepsPerClock;  // follow mode: the microsteps a master clock spans, as of the step rate at the last step boundary
  std::uint16_t silent_ = 0;           // bit per track: its MIDI notes are held back (refresh_silence)
  std::uint16_t mq_pending_ = 0;       // bit per track: a mute toggle waits for its boundary (render context)
  std::uint16_t mq_hold_ = 0;          // bit per track: the effective mute until then
  std::uint8_t mq_kind_[kMaxTracks] = {};  // MuteQuant of each pending toggle
  std::atomic<std::uint16_t> pending_pub_{0};              // mq_pending_ for the main loop: the render is the one writer
  std::atomic<std::uint8_t> activity_[kMaxTracks] = {};    // note_count: the render is the one writer

  // external clock
  const midi::ClockDll* follow_ = nullptr;
  core::SQ32 follow_offset_ = 0;
  std::uint64_t follow_base_tick_ = 0;
  std::uint64_t last_follow_abs_ = 0;
  bool ext_start_ = false;             // the pending start or continue is the master's (apply(TransportEvent)): its clock number is follow_base_tick_ (OS-005c2)

  Stats stats_{};
  audio::PlayPosition published_;      // written at the end of step(), the one writer; read by the main loop
};

}  // namespace o3os::seq
