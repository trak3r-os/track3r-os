// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The performance mode's slots (OS-011b; triage BASE-20 "Performance mode: 12 FX slots, three control layers"; docs/design/product-ux.md section 4, Perform): the model of the
// performance mode as a pure bank, and what it asks of the engine. The third control layer itself (the one
// that wins over the two sequencer columns and lets the parameter return to them when it ends) is seq/control.h and the voices; this is the Perform screen's side of it.
//
// BEHAVIOUR:
//   - twelve SLOTS, each holding one of 21 performance FX (the first twelve in order by default) and four values: row 0 is "off",
//     rows 1..3 are values the user keeps (all 0). The 48 pads are the slots' four rows: pad n is slot n % 12, row n / 12.
//   - A pad pressed makes its row the slot's selected one, holds the slot, and loads the row's value on every AFFECTED track; a release re-loads it and lets go of the hold
// The row stays selected after the release, so the FX stays on until another row (or row 0, which is off) of the slot is pressed. Row 0 ends the FX.
//   - Setting a slot's FX ends every other slot that holds the same FX, and the FX it blocks (the three filters block each other).
//   - The encoder changes the value of the slots that are held and not on row 0, by the detents, held to the FX's range; in the EDIT state (Rec toggles it) the new
//     value is stored in the row, otherwise it is only the live one and the next press of the row loads the stored one.
//   - The AFFECTED tracks (eight: the track buttons toggle them): a track that becomes affected gets every slot's FX at once, one that stops gets them all ended.
//   - A value of 0 is "no FX": the slot ends the FX instead of setting 0. That fits every FX of the table: for the labelled ones 0 is the first label
//     (stutter Off, play mode forward, pattern length "---").
//   - In EDIT the arrows move the edit place, Up / Down change the slot's FX (wrapping, clearing its values), Shift + arrows swap two slots, Copy / Shift + Copy copy and paste a
//     slot.
//   - Leaving the mode ends every FX on every track (the Master and Song buttons keep it).
//
// WHAT IS NEW (Community OS behaviour). The bank keeps the DESIRED state and RECONCILES it with what it last told the
// engine (apply): every operation updates the slots and then calls apply(), which emits only the commands that bring each track's FX to what the slots say, as seq::Command
// on the sequencer's ring (PerfControl for a parameter group of the control table, PerfStutter, PerfTrack for the play mode, PerfPatternLen). A command the full ring refused is
// simply sent again at the next apply (the App calls it once a pass), so the engine and the screen cannot drift. The commands are drained at the start of a block
// (Sequencer::step), arbitrated by the control table and bounded by the ring and by kMaxCommandsPerApply; nothing here allocates, blocks or loops unboundedly.
//
// WHICH FX THE ENGINE ACTS ON. `live` in the table: today the voices act on the performance layer's Filter group (the three filter FX: voices.h PERFORMANCE FILTER) and on its
// Bit depth and Overdrive groups (BITDEPTH and OVERDRV, OS-PERFFX P1: voices.h PERFORMANCE LAYER), the strips on VOLUME, PANNING, DELAY and REVERB (OS-PERFFX P2, P3:
// Graph::set_perf_gain / set_perf_pan / set_perf_send; VOLUME only cuts, values from 0 up are 0 dB; DELAY and REVERB are an offset of the track's send, -100 closes it and +100 opens it
// whole) and the sequencer on the stutter, the play mode and the pattern length. TUNE (OS-PERFFX P5, FxGroup::Tune, no step FX has the group) is the sampler voices' pitch base in semitones, -48..48
// (voices.h PERFORMANCE LAYER, TUNE; the Amiga voices and the module instruments do not follow it). SMP POS and SMP END (OS-PERFFX P7, FxGroup::Position / SampleEnd) are the sampler voices' first and last frame, in percent of
// the part, read at a note's start (voices.h PERFORMANCE LAYER, SMP POS AND SMP END). PLAYBACK (OS-PERF-REVERSE, P8, FxGroup::Reverse) plays the sampler voices' next notes backward (voices.h PERFORMANCE LAYER, PLAYBACK). The others (the five LFO speeds) go to the control table too (the
// arbitration and the layers are generic) but no voice reads them yet, and the screen shows them dimmed: no parity is claimed for them.
// Per-track source patterns (track buttons with the encoder) and the performance tempo are not here: their behaviour (PAR-029, PAR-030, PAR-077) is for a
// later slice to specify.
//
// OS-PERF2 (new Community OS behaviour, not in the official manual, official-manual-map; Perform F1..F8; the scenes and LATCH are the project's since OS-PERSIST2, the SCEN chunk): three SCENES (a snapshot of the twelve slots' FX, selected rows and stored
// values, taken with store_scene and put back with recall_scene: the affected tracks, the held pads and the EDIT state are not part of it), a LATCH switch (on, the default: a slot's row stays
// selected after the pad is let go, as the official manual describes, not verified; off: a pad's row ends with the release), RESET (every slot off at once, the affected tracks kept) and the step RATE of half / double time, which the bank
// reconciles with the sequencer like any FX (Command::PerfRate, see sequencer.h HALF / DOUBLE TIME). The scene is a plain struct of model::PerfState, so a later format slice can write it.
//
// Numbers: 12 slots x (1 + 1 + 8) bytes, the live values, the copy buffer, 12 tracks x 22 FX last-sent values, three scenes of 120 bytes: about 1 KB.

#include <o3os/model/project_state.h>
#include <o3os/seq/fx.h>
#include <o3os/seq/sequencer.h>

#include <cstdint>
#include <type_traits>

namespace o3os::seq {

inline constexpr unsigned kPerfSlots = 12;
inline constexpr unsigned kPerfRows = 4;   // row 0 off, rows 1..3 the values
inline constexpr unsigned kPerfTracks = 12; // the tracks the layer can affect: the 12 a project has (F1..F8 reach 1..8, the pads of OS-PADS the rest). Live state, not stored (o3-project.md, PERF)
inline constexpr unsigned kPerfPads = kPerfSlots * kPerfRows;
inline constexpr unsigned kPerfScenes = 3;  // F1..F3 (OS-PERF2)

// A scene: the slots as a project holds them (model::PerfState: FX, selected row, stored values) and whether anything was stored. It is the project's own plain struct (model::SceneState: the
// SCEN chunk of OS-PERSIST2 writes it as it is), so that what the bank holds and what the file says are one thing.
using PerfScene = model::SceneState;
static_assert(std::is_trivially_copyable_v<PerfScene> && kPerfScenes == model::kSceneCount, "a scene is plain data (the SCEN chunk writes it)");

// The performance FX, in their order. The values are the ids a slot stores.
enum class PerfFx : std::uint8_t {
  None = 0,
  Volume,
  Panning,
  Tune,
  LowPass,
  HighPass,
  BandPass,
  DelaySend,
  ReverbSend,
  SamplePosition,
  SampleEnd,
  SamplePlayback,
  VolumeLfoSpeed,
  PanningLfoSpeed,
  FinetuneLfoSpeed,
  FilterLfoSpeed,
  GranWtLfoSpeed,
  StepRepeater,
  PatternPlayMode,
  PatternLength,
  BitDepth,   // OS-PERFFX P1 (new Community OS behaviour): appended, so that every id stored in a project keeps its meaning
  Overdrive,
  Count
};
inline constexpr unsigned kPerfFxCount = static_cast<unsigned>(PerfFx::Count);

struct PerfFxInfo {
  const char* name;       // upper case, at most 8 characters (a slot's column is 65 px of the 800 px panel in the Label face)
  std::int16_t min;       // the range of the value 
  std::int16_t max;
  FxGroup group;          // the control table's group; None for the sequencer's own FX and for the two the model has no group for
  PerfFx blocks[2];       // setting this ends these
  bool live;              // something in the engine acts on it (see above)
  const char* const* labels;  // the names of the values 0..max for a labelled FX, else nullptr
};

// The slot labels (sample playback, stutter, pattern play mode and pattern length), except the play mode's: its stored value 0..23 is seq::TrackPlayMode 0..23 (PerfTrack in
// sequencer.cpp), so each label names the mode that runs: 0..3 FWD, BACK, PING, RND (OS-BUG-PERFMODE) and 4..23 the custom play orders C1..C20 (OS-PERFMODES M2, M4).
inline constexpr const char* kPerfLabelsPlayback[2] = {">>>", "<<<"};
inline constexpr const char* kPerfLabelsStutter[16] = {"OFF", "16", "12", "8", "6", "4", "3", "2", "1", "1/2", "1/3", "1/4", "1/6", "1/8", "1/12", "1/16"};
inline constexpr const char* kPerfLabelsPlayMode[24] = {"FWD", "BACK", "PING", "RND", "C1", "C2", "C3", "C4", "C5", "C6", "C7", "C8", "C9", "C10", "C11", "C12", "C13", "C14", "C15", "C16", "C17", "C18", "C19", "C20"};
inline constexpr const char* kPerfLabelsLength[9] = {"---", "1", "2", "4", "8", "16", "32", "64", "128"};

// The ranges of the LFO speeds: 23 (amp envelope) and 28 (other envelopes).
inline constexpr PerfFxInfo kPerfFxInfo[kPerfFxCount] = {
    {"", -100, 100, FxGroup::None, {PerfFx::None, PerfFx::None}, false, nullptr},
    {"VOLUME", -100, 100, FxGroup::Volume, {PerfFx::None, PerfFx::None}, true, nullptr},
    {"PANNING", -100, 100, FxGroup::Panning, {PerfFx::None, PerfFx::None}, true, nullptr},
    {"TUNE", -48, 48, FxGroup::Tune, {PerfFx::None, PerfFx::None}, true, nullptr},
    {"LOWPASS", -100, 100, FxGroup::Filter, {PerfFx::HighPass, PerfFx::BandPass}, true, nullptr},
    {"HIPASS", -100, 100, FxGroup::Filter, {PerfFx::LowPass, PerfFx::BandPass}, true, nullptr},
    {"BANDPASS", -100, 100, FxGroup::Filter, {PerfFx::LowPass, PerfFx::HighPass}, true, nullptr},
    {"DELAY", -100, 100, FxGroup::DelaySend, {PerfFx::None, PerfFx::None}, true, nullptr},
    {"REVERB", -100, 100, FxGroup::ReverbSend, {PerfFx::None, PerfFx::None}, true, nullptr},
    {"SMP POS", -100, 100, FxGroup::Position, {PerfFx::None, PerfFx::None}, true, nullptr},  // OS-PERFFX P7: the sampler voices' start frame at a note
    {"SMP END", -100, 100, FxGroup::SampleEnd, {PerfFx::None, PerfFx::None}, true, nullptr},  // OS-PERFFX P7: and their last frame
    {"PLAYBACK", 0, 1, FxGroup::Reverse, {PerfFx::None, PerfFx::None}, true, kPerfLabelsPlayback},  // OS-PERF-REVERSE (P8): 1 plays a one shot or beat slice note backward (the step FX 26 shares the group)
    {"VOL LFO", -23, 23, FxGroup::LfoVolume, {PerfFx::None, PerfFx::None}, true, nullptr},
    {"PAN LFO", -28, 28, FxGroup::LfoPanning, {PerfFx::None, PerfFx::None}, true, nullptr},
    {"TUNE LFO", -28, 28, FxGroup::LfoFinetune, {PerfFx::None, PerfFx::None}, true, nullptr},
    {"FLT LFO", -28, 28, FxGroup::LfoFilter, {PerfFx::None, PerfFx::None}, true, nullptr},
    {"POS LFO", -28, 28, FxGroup::LfoPosition, {PerfFx::None, PerfFx::None}, true, nullptr},
    {"STUTTER", 0, 15, FxGroup::None, {PerfFx::None, PerfFx::None}, true, kPerfLabelsStutter},
    {"PLAYMODE", 0, 23, FxGroup::None, {PerfFx::None, PerfFx::None}, true, kPerfLabelsPlayMode},  // OS-PERFMODES M4: 4..23 are the custom play orders C1..C20 (seq::TrackPlayMode, play_orders.h)
    {"PAT LEN", 0, 8, FxGroup::None, {PerfFx::None, PerfFx::None}, true, kPerfLabelsLength},
    // OS-PERFFX P1: the value is how many bits are taken away (16 - bits: 1 is 15 bits, 15 is 1 bit); 0 ends the FX, which is the clean sound. No labels: the screen shows the number.
    {"BITDEPTH", 0, 15, FxGroup::BitDepth, {PerfFx::None, PerfFx::None}, true, nullptr},
    {"OVERDRV", 0, 100, FxGroup::Overdrive, {PerfFx::None, PerfFx::None}, true, nullptr},
};

constexpr const PerfFxInfo& perf_info(PerfFx fx) noexcept { return kPerfFxInfo[static_cast<unsigned>(fx) < kPerfFxCount ? static_cast<unsigned>(fx) : 0u]; }

// The text of a value: "---" for 0, the label of a labelled FX, else the signed whole number. At most 7 characters and the NUL.
unsigned perf_value_text(PerfFx fx, int value, char (&out)[8]) noexcept;

// The pattern length a PatternLength value stands for (-1, 1, 2, 4, ... 128): 0 is "none", the Command's own.
constexpr std::uint32_t perf_length_steps(int value) noexcept { return value <= 0 ? 0u : (value > 8 ? 128u : (1u << static_cast<unsigned>(value - 1))); }

// What the engine is told: one Command at a time, false when the ring had no room (the bank then tries again at the next apply). The platform's sink is the sequencer's
// command ring; a test's is a vector.
class CommandSink {
 public:
  virtual bool push(const Command& command) noexcept = 0;

 protected:
  CommandSink() = default;
  ~CommandSink() = default;
};

// The Command that sets an FX on a track (`value` not 0) or ends it (`value` 0). `track` is 0..7. False (nothing in `out`) for an FX with nothing to send: None, Sample end.
// A per-track FX and a group of the control table go as the track's own command; the pattern length is the sequencer's global (PerfPatternLen: the track does not matter).
bool perf_command(unsigned track, PerfFx fx, int value, Command& out) noexcept;

// The most commands one apply() sends, so that a burst (eight tracks joining three FX each) leaves the sequencer's ring of 32 room for the transport: the rest goes out over the next passes.
inline constexpr unsigned kMaxCommandsPerApply = 16;

class PerformBank {
 public:
  PerformBank() noexcept { reset(); }

  // The defaults (ours): slot i holds FX i + 1 with that FX's three default values (model::kPerfDefaultValues, docs/specs/OS-PERFFX.md section 9), nothing selected, no track affected, edit off. The engine is not told (it holds nothing either).
  void reset() noexcept;

  // ---- the project's part (OS-004d, model/project_state.h): the slots' FX, rows and stored values. load_slots() puts a project's slots in: every FX of the bank ends (nothing is held, no track
  // is affected, the edit state is off: the engine is told by the next apply()), an FX id the table does not have becomes none and each value is held to its FX's range, so state() of
  // a bank that was loaded is what the bank holds, not necessarily what was loaded. The scenes are cleared (runtime state of the session).
  model::PerfState state() const noexcept;
  void load_slots(const model::PerfState& s) noexcept;
  // ---- OS-PERSIST2: the scenes and LATCH are the project's too (the SCEN chunk). scenes_state() is what the bank holds; load_scenes() puts a project's in, AFTER load_slots() (which clears the
  // scenes): the scenes are copied as they are (recall_scene holds a value to its FX's range whatever a scene says), LATCH as given, nothing is told to the engine (a scene is not applied by loading it).
  model::ScenesState scenes_state() const noexcept;
  void load_scenes(const model::ScenesState& s) noexcept;

  // ---- the slots
  PerfFx fx(unsigned slot) const noexcept { return slot < kPerfSlots ? static_cast<PerfFx>(slots_[slot].fx) : PerfFx::None; }
  unsigned row(unsigned slot) const noexcept { return slot < kPerfSlots ? slots_[slot].row : 0u; }
  // The stored value of a row (rows 1..3; row 0 has none).
  int stored(unsigned slot, unsigned row) const noexcept { return slot < kPerfSlots && row < kPerfRows ? slots_[slot].value[row] : 0; }
  // The value the slot has now (the stored one of its row, or what the encoder made of it), 0 when its row is 0.
  int live(unsigned slot) const noexcept { return slot < kPerfSlots ? live_[slot] : 0; }
  bool held(unsigned slot) const noexcept { return slot < kPerfSlots && ((held_ >> slot) & 1u) != 0u; }
  bool affected(unsigned track) const noexcept { return track < kPerfTracks && ((affected_ >> track) & 1u) != 0u; }
  std::uint16_t affected_mask() const noexcept { return affected_; }
  bool edit() const noexcept { return edit_; }
  unsigned edit_slot() const noexcept { return edit_slot_; }
  // The slot's FX is on: a row but 0 is selected and its value is not 0 (what the pads' LEDs and the screen's highlight show); a slot in CC mode
  // (OS-MIDICCMAP S3) is on when its live value is not 0, whatever its row.
  bool active(unsigned slot) const noexcept {
    return slot < kPerfSlots && live_[slot] != 0 && (slots_[slot].row != 0u || ((cc_mask_ >> slot) & 1u) != 0u);
  }
  // OS-MIDICCMAP S3 (3.4): the slot is driven by a mapped CC (51..62). A runtime bit, never stored: state() and the scenes do not see it.
  bool cc_mode(unsigned slot) const noexcept { return slot < kPerfSlots && ((cc_mask_ >> slot) & 1u) != 0u; }

  // ---- the live side. Each updates the slots, then reconciles with the engine (apply); true when something the screen shows changed.
  // A pad (0..47) went down / up: slot pad % 12, row pad / 12.
  bool pad_down(unsigned pad, CommandSink& out) noexcept;
  bool pad_up(unsigned pad, CommandSink& out) noexcept;
  // The encoder: `detents` on the value of every slot that is held and not on row 0 (stored too in the EDIT state); a held slot in CC mode is turned as for a row.
  bool turn(int detents, CommandSink& out) noexcept;
  // OS-MIDICCMAP S3 (3.4): a mapped CC 51..62 on the slot (`v` the CC value 0..127): the slot enters CC mode, its live value becomes its place in its
  // FX's range (min + v * (max - min) / 127, the official law of F5), the slots holding the same FX end (exclude) and apply() sends it to the affected
  // tracks. The stored row and values are untouched, so the PERF chunk and the scenes never see CC mode. A slot with no FX ignores it.
  // CC mode ends on: a pad of that slot, RESET (all_off), leaving the screen (turn_off), a scene recall, change_fx, paste_slot, swap_slots of that slot, load_slots.
  bool set_cc(unsigned slot, unsigned v, CommandSink& out) noexcept;
  // A track button: the track joins or leaves the layer.
  bool toggle_track(unsigned track, CommandSink& out) noexcept;
  // Entering the mode: the live values are the stored ones again, nothing is held.
  void enter() noexcept;
  // Leaving it: no track is affected, nothing is held, and every FX ends. The rows stay selected.
  bool turn_off(CommandSink& out) noexcept;

  // ---- OS-PERF2: scenes, latch, reset, step rate. Each live one reconciles with the engine (apply); true when something the screen shows changed.
  // A scene (0..kPerfScenes-1): store_scene snapshots the slots (FX, rows, stored values: not the live values the encoder made, not the affected tracks); recall_scene puts them back (each
  // value held to its FX's range, the live values are the stored ones again, held pads stay held) and the engine is told the difference. An empty scene recalls nothing (false).
  const PerfScene& scene(unsigned i) const noexcept { return scenes_[i < kPerfScenes ? i : 0u]; }
  bool scene_stored(unsigned i) const noexcept { return i < kPerfScenes && scenes_[i].stored != 0u; }
  bool scene_matches(unsigned i) const noexcept;  // stored, and the slots are what it holds (the screen lights the scene in force)
  bool store_scene(unsigned i) noexcept;
  bool recall_scene(unsigned i, CommandSink& out) noexcept;
  // LATCH: on (the default; the official manual describes the effect staying on, not verified); off, the release of a pad ends its slot's FX when the pad's row is the slot's selected one. Switching does not touch the slots (RESET does).
  bool latch() const noexcept { return latch_; }
  void set_latch(bool on) noexcept { latch_ = on; }
  // RESET: every slot off (row 0, nothing held); the slots' FX and stored values, the affected tracks and the EDIT state stay.
  bool all_off(CommandSink& out) noexcept;
  // The step rate: 0 normal, 1 half time, 2 double time (anything else is normal). The held keys' effect: Perform F4 / F5 set it, the bank sends it with the rest at the next apply.
  unsigned rate() const noexcept { return rate_; }
  void set_rate(unsigned rate) noexcept { rate_ = rate == 1u ? std::uint8_t{1} : (rate == 2u ? std::uint8_t{2} : std::uint8_t{0}); }
  // The loop length the PAT LEN FX has the sequencer play (steps), 0 when none: what the sequencer was last told. OS-DJ B2 counts the time to the end of the loop with it.
  std::uint32_t told_length() const noexcept { return perf_length_steps(sent_length_); }

  // ---- the EDIT state (Rec): which slot the arrows are on, its FX, the order of the slots.
  void set_edit(bool on) noexcept { edit_ = on; }
  bool move_edit(int delta) noexcept;                          // the edit place, held to 0..11
  bool change_fx(unsigned slot, int delta, CommandSink& out) noexcept;  // the slot's FX +- 1, wrapping; its row goes to 0 and its values to the new FX's defaults (model::kPerfDefaultValues)
  bool swap_slots(unsigned a, unsigned b, CommandSink& out) noexcept;   // the FX and the four values (not the rows)
  void copy_slot(unsigned slot) noexcept;
  bool paste_slot(unsigned slot, CommandSink& out) noexcept;

  // ---- OS-DJ B1 (new Community OS behaviour, docs/specs/OS-DJ.md): a layer above the slots for the DJ page's row 2, the group's REP (the stutter), ECHO (the DELAY send offset, +100) and CUT (VOLUME -100).
  // While a track's layer value is not 0 it wins over the slots' FX of the same kind, whether or not the track is AFFECTED; at 0 the slots' FX is back (apply() sends the slot's value again). Runtime only,
  // never stored (state() and the scenes do not see it); the DJ page sets it every pass and zeroes it when the page is left. True when something changed (call apply()).
  bool set_dj_layer(unsigned track, int stutter, int delay, int gain) noexcept;

  // ---- the reconciliation. Sends what differs between the slots' state and what the engine was last told, at most kMaxCommandsPerApply commands, none past the first refusal
  // of the sink. True when everything is told (nothing differs). The App calls it once a pass, so a refused command is not lost.
  bool apply(CommandSink& out) noexcept;

 private:
  struct Slot {
    std::uint8_t fx;
    std::uint8_t row;
    std::int16_t value[kPerfRows];
  };

  void load_row(unsigned slot) noexcept;
  void end_slot(unsigned slot) noexcept;           // held, row and live value to 0
  void exclude(unsigned slot) noexcept;            // the slots that hold the same FX or one it blocks end
  int desired(unsigned track, unsigned fx) const noexcept;

  Slot slots_[kPerfSlots];
  std::int16_t live_[kPerfSlots];
  std::uint16_t held_;
  std::uint16_t cc_mask_;  // OS-MIDICCMAP S3: bit s, slot s is in CC mode (runtime only, never stored; 2 B of SRAM)
  std::uint16_t affected_;
  bool edit_;
  std::uint8_t edit_slot_;
  Slot copy_;
  bool copied_;
  PerfScene scenes_[kPerfScenes];
  bool latch_;
  std::uint8_t rate_;       // the step rate wanted (set_rate) ...
  std::uint8_t sent_rate_;  // ... and the one the sequencer was last told
  std::int16_t sent_[kPerfTracks][kPerfFxCount];  // what each track's FX were last told to be (0: ended)
  std::int16_t sent_length_;                       // the pattern length FX's value the sequencer was last told
  struct DjLayer {
    std::int8_t stutter;  // 0..15 (PerfFx::StepRepeater), 0 none
    std::int8_t delay;    // 0 none, 100 (PerfFx::DelaySend)
    std::int8_t gain;     // 0 none, -100 (PerfFx::Volume)
  };
  DjLayer dj_[kPerfTracks];  // main loop only
};

}  // namespace o3os::seq
