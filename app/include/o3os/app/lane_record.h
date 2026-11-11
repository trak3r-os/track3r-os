// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-MIDIREC-P1 S3 (docs/specs/OS-MIDIREC-P1.md, "S3 as built"): the main loop's side of a Lane recording. NEW COMMUNITY OS BEHAVIOUR. With REC STORE = Lane, the Pattern screen in LIVE (pattern_recording) and
// the cursor's instrument a MIDI one, the App ARMS the keyboard's ring (midi::CcInput::open_take): the render then stamps every note and control change with the recording track's tick (A8) and the main
// loop drains the stamped copies into the take buffer (model::LaneTake: thinning, A9) instead of writing the grid. When the recording ends (or the track, the instrument or the pattern changes, or the buffer
// fills) the take is merged into the lane (Editor::lane_merge_take, A10/A11) as one undo unit, inside the undo group of the take (rec_take_sync) when that is open. With REC STORE = Grid (the default)
// nothing here runs: lane_sync finds it not wanted and the keyboard's notes go the way they always did.
//
// Main loop only. The App owns one LaneRec; the take buffer itself (model::LaneTake, 16 KiB) is placed in SDRAM by the platform and attached (App::attach_lane_take).

#include <o3os/core/param.h>

#include <cstdint>

namespace o3os::midi {
class CcInput;
}
namespace o3os::model {
class Editor;
class LaneTake;
}

namespace o3os::app {

struct Model;
struct ViewState;

// Stamped messages taken from the ring in one pass of the main loop (the ring holds midi::CcInput::kStampRingSize; the rest waits).
inline constexpr unsigned kLaneStampPerPass = 64;

struct LaneRec {
  static constexpr std::uint8_t kNone = 0xFF;
  model::LaneTake* take = nullptr;  // attach_lane_take(): null, and nothing here runs
  midi::CcInput* in = nullptr;      // the keyboard's ring, seen at the first poll of the main loop
  std::uint8_t track = kNone;       // the armed track (the render stamps for it); kNone: not armed
  std::uint8_t channel = 0;         // of the armed instrument (0..15) and its port (the router's bits): what the take's events carry
  std::uint8_t port = 0;
  std::uint8_t pattern = 0;         // the pattern of the take in progress
  bool retry = false;               // a finished take was refused (no room in the undo ring or the pool) and is still held: one more try when recording next begins
  bool said_drop = false;
  std::uint32_t last_tick = 0;      // the tick of the last message taken
  std::uint32_t last_pos = 0;       // the playhead's tick on the track at the last poll: the end tick when recording stops (the transport keeps no stop position)
  std::uint32_t stamp_drops = 0;    // midi::CcInput::stamp_dropped() at the last look
  // S4 (A13): the running value of each CC letter A..F for the moves that have no absolute position (the encoder, a relative binding): the parameter's own units, cc_have bit n = letter n has one
  // (the first move of a letter starts at CC 64, never at the selector). Main loop only; reset when a take is armed.
  float cc_now[6] = {};
  std::uint8_t cc_have = 0;
  bool said_cc = false;             // this recording has said once that a letter records nothing (PROG, CH AT, POLY AT, unassigned)
  bool said_ci = false;             // ... and once that a move in the count-in is not recorded
  bool run = false;                 // a count-in or a take is under way: said_cc / said_ci belong to it (not reset when the take is armed out of the count-in)
  // S5, STEP mode (REC, no stamps): the chord being held. Its notes go into the take as they are pressed (LEN one step, at the cursor step's first tick); the last key up commits it and moves the cursor on.
  bool step_open = false;
  std::uint8_t step_n = 0;          // keys of the chord still down
  std::uint8_t step_pattern = 0;
  std::uint8_t step_track = 0;
  std::uint8_t step_channel = 0;
  std::uint8_t step_port = 0;
  std::uint32_t step_tick = 0;      // the cursor step's first tick, fixed when the chord began (the cursor may move while it is held)
  std::uint32_t step_held[4] = {};  // 128 bits: the keys down

  // The keyboard's notes and control changes are going to the take (the grid does not take them).
  bool capturing() const noexcept { return track != kNone; }
};

// One poll of the main loop, called by App::poll_midi_notes before the grid's note entry (so a commit joins the take's undo group while it is still open). Arms or disarms the keyboard's ring as the state says, drains
// the stamped messages into the take and commits it at the end. True: the keyboard's notes are in the take and the grid does not write them. `in_port` / `in_channel` are MIDI IN and IN CHANNEL (midi_in_accepts).
bool lane_poll(LaneRec& rec, midi::CcInput& in, model::Editor* editor, const Model& model, ViewState& view, unsigned in_port, unsigned in_channel) noexcept;

// OS-MIDIREC-P1 S4 (A13,"S4 as built, part b"): while a Lane take is being recorded, a move of the CC letter parameter `id` (CC A..F of the cursor's MIDI instrument: a Learn binding, or the encoder of
// Instr + FX1) does not change which controller the letter sends: it is the controller's value. lane_cc_letter: the letter 0..5 when the lane is capturing and `id` is a CC row of the selected instrument,
// else -1 (the move is an ordinary parameter edit). lane_cc_move puts the value (0..127) into the take for the controller the letter names NOW (instrument midi_cc[letter], or the project's Config number),
// through the take's thinning, at the playhead of the last poll. PROG, CH AT, POLY AT and an unassigned letter record nothing and say so once per recording (true: the move was handled all the same).
// False: the move could not be put anywhere (no take, or the count-in, which has no playhead on the track): the caller says so and still swallows it.
int lane_cc_letter(const LaneRec& rec, const Model& model, const ViewState& view, core::ParamId id) noexcept;
bool lane_cc_move(LaneRec& rec, model::Editor* editor, const Model& model, ViewState& view, unsigned letter, unsigned value) noexcept;

// S5, STEP mode (REC = Edit with the cursor free, stopped or playing; REC STORE = Lane, the cursor's instrument a MIDI one, a single cell selected): a key down enters its note into the lane at the first tick
// of the cursor's step with LEN 48 (one step) and joins the chord; when the last key of the chord is up the chord is merged (one undo unit, the same merge as a take) and the cursor moves on by the edit step.
// True: the note is the lane's (the grid does not write it). `note` is the key after SCALE LOCK; `velocity` the keyboard's.
bool lane_step_on(LaneRec& rec, model::Editor* editor, const Model& model, ViewState& view, std::uint8_t note, std::uint8_t velocity) noexcept;
// True when `note` was a key of the open chord (the grid's note-off does not run for it).
bool lane_step_off(LaneRec& rec, model::Editor* editor, const Model& model, ViewState& view, std::uint8_t note) noexcept;
// All notes off from the keyboard: an open chord is committed as it is.
void lane_step_all_off(LaneRec& rec, model::Editor* editor, const Model& model, ViewState& view) noexcept;

// A new document, or the editor goes away: the take in progress and a held take are dropped, the ring is disarmed (no commit).
void lane_discard(LaneRec& rec) noexcept;

}  // namespace o3os::app
