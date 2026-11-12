// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The pads' mode on the Pattern screen and the momentary TRACKS layer (OS-PADS-A; new Community OS behaviour, the maintainer's design of 2026-10-12/13).
//
//   NOTES    today's pads: preview in Browse, a note, an instrument, a volume or an FX value in Edit, by the column under the cursor.
//   PIANO    OS-PADS-B (pad_maps.h): two octaves drawn as a piano, the pattern jumps on column 1 and the octave keys on column 12; the pads play and enter notes (action pads.play).
//   ISO      OS-PADS-B (pad_maps.h): every column a semitone, every row up a fourth (+5); a pad in the scale Front, one outside it Back (OS-NOTESLED2).
//   STEP     OS-PADS-B (pad_mode_actions.cpp): the pads are the steps of the cursor's track; see there.
//   PATS     OS-PADS-B (pad_mode_actions.cpp): the pads are patterns 1..48: lit = has content, Front = playing, blinking = queued; a tap queues it (stopped: selects it).
//   VU       OS-PADS-B (pad_lights.h): the 12 columns are the 12 tracks, a 4-row level meter each (the engine's published meters). Shift + F7 again: FADERS, the same pads as 12 track
//            volumes (a tap on row n sets the level in 4 steps); again: back to VU.
//   TRACKS   the 12 columns are the 12 tracks, the four rows TRACK / MUTE / MUTE MODE / SOLO (on the Perform screen, AFFECTED / MUTE / MUTE MODE / SOLO): app/pad_lights.h.
//
// OS-SONGPADS (docs/specs/SONG-UX.md section 2): the Song screen has a mode pair of its own, TRACKS (this layer, in force on Song) and SLOTS (the 48 pads are 48 slots of the order list). Shift + F7 switches; TRACKS
// at boot; not saved. The state lives here with the rest of the pads' modes (`song_slots()`), the actions are song_actions.cpp's, the lights SongScreen::pad_roles.
//
// Shift picks. A LONE Shift hold (Pattern and Perform, and Song in its SLOTS mode) is TRACKS on the pads for as long as it is held, and the release is back to the mode before it. Any other control used while
// Shift is down (an arrow, Insert, Delete, an F-key, the encoder: a chord) makes the layer yield for the rest of that hold: Shift then only modifies, as it always did. A pad is not
// a chord, it is what the layer is for. Shift + F2..F7 choose the mode that stays (OS-PADOCT: F2 NOTES, F3 PIANO, F4 ISO, F5 TRACKS, F6 PATS, F7 VU / FADERS; F1 is OCTAVE, it cycles the base C of the NOTES, PIANO and ISO pads, pads.octave; there is no key for the STEP mode below, which is kept, OS-STEPPADOFF, until OS-STEPPAD) and show the picker on the key row.
// Runtime only: nothing here is saved (OS-PERSIST2 left it out on purpose: the mode is a performer's layout, a device preference, whose home is a row of the settings file and a page of
// the Config screen). Main loop only, no heap.

#include <cstdint>

namespace o3os::app {

enum class PadMode : std::uint8_t { Notes = 0, Piano = 1, Tracks = 2, Iso = 3, Steps = 4, Patterns = 5, Vu = 6 };
inline constexpr unsigned kPadModes = 7;
inline constexpr unsigned kPadPickerKeys = 7;  // an upper bound of the keys of a pick (the PADS picker, OS-UXF-PAT 7, has F1..F6 and F8 BACK); the pad-modes test probes the keys past the modes with it

inline constexpr const char* kPadModeNames[kPadModes] = {"NOTES", "PIANO", "TRACKS", "ISO", "STEP", "PATS", "VU"};
// The modes with a layer of their own over the NOTES pads, other than TRACKS (which has its own): the context PadsMode is in the chain for them (OS-PADS-B).
constexpr bool pad_mode_has_layer(PadMode m) noexcept { return m == PadMode::Piano || m == PadMode::Iso || m == PadMode::Steps || m == PadMode::Patterns || m == PadMode::Vu; }
// The modes whose pads are not notes: their releases are the layer's alone (no live note to end, no Perform slot, no Rec note-off), see PadLayer::pad_down.
constexpr bool pad_mode_takes_pads(PadMode m) noexcept { return m == PadMode::Steps || m == PadMode::Patterns || m == PadMode::Vu; }

class PadLayer {
 public:
  // The mode that stays (chosen with Shift + F1..F3).
  PadMode fixed() const noexcept { return fixed_; }
  // VU mode's second page (Shift + F7 again): the same pads as faders.
  bool faders() const noexcept { return fixed_ == PadMode::Vu && faders_; }
  // What the info line says for a mode that stays.
  const char* name() const noexcept { return faders() ? "FADERS" : kPadModeNames[static_cast<unsigned>(fixed_)]; }
  bool shift_held() const noexcept { return shift_; }
  // Shift is down and nothing else has been used since it went down.
  bool momentary() const noexcept { return shift_ && !yielded_; }
  // The mode the pads are in now: TRACKS while a lone Shift is held, else the fixed one.
  PadMode effective() const noexcept { return momentary() ? PadMode::Tracks : fixed_; }
  // True when the pads of the screen should act as the TRACKS layer (on the Pattern screen, a fixed or a momentary TRACKS; on the Perform screen the momentary one only).
  bool tracks(bool pattern_screen) const noexcept { return pattern_screen ? effective() == PadMode::Tracks : momentary(); }
  // OS-SONGPADS: Song's pads. SLOTS (Shift + F7) is the screen's own mode; TRACKS is the layer, in force there whenever SLOTS is not, and for as long as a lone Shift is held in SLOTS (D9).
  bool song_slots() const noexcept { return song_slots_; }
  bool song_tracks() const noexcept { return !song_slots_ || momentary(); }
  bool toggle_song_slots() noexcept {
    song_slots_ = !song_slots_;
    return song_slots_;
  }

  // Shift went down or up. True when what the pads mean changed.
  bool shift_changed(bool held) noexcept {
    if (shift_ == held) {
      return false;
    }
    const PadMode before = effective();
    shift_ = held;
    yielded_ = false;  // a new hold starts lone, the release ends it
    return effective() != before;
  }
  // Another button (a press) or the encoder was used. While Shift is down this is a chord: the momentary layer yields until Shift is let go. True when the pads' meaning changed.
  bool other_input() noexcept {
    if (!shift_ || yielded_) {
      return false;
    }
    const PadMode before = effective();
    yielded_ = true;
    return effective() != before;
  }
  // The PADS picker's keys (Shift + F2, then F1..F6) choose the mode (which stays), `key` being the mode's number 0..6 (PadMode); VU again toggles VU / FADERS. True when the fixed mode changed.
  bool pick(unsigned key) noexcept {
    if (key >= kPadModes) {
      return false;
    }
    const PadMode to = static_cast<PadMode>(key);
    if (to == fixed_) {
      faders_ = to == PadMode::Vu && !faders_;  // pressing VU again is the press-again-for-the-next-page rule of the Record screen
      return to == PadMode::Vu;
    }
    faders_ = false;
    fixed_ = to;
    return true;
  }

  // A pad went down (`tracks_layer`: the TRACKS layer took it) or up. The release of a pad the layer took is the layer's: the screen's own pad handling (a Perform slot's release, a recorded
  // note's end) must not see it, which pad_up() answers with true.
  void pad_down(unsigned pad, bool tracks_layer) noexcept {
    if (pad < 64u) {
      taken_ = tracks_layer ? (taken_ | (std::uint64_t{1} << pad)) : (taken_ & ~(std::uint64_t{1} << pad));
    }
  }
  bool pad_up(unsigned pad) noexcept {
    if (pad >= 64u) {
      return false;
    }
    const std::uint64_t bit = std::uint64_t{1} << pad;
    const bool was = (taken_ & bit) != 0u;
    taken_ &= ~bit;
    return was;
  }

 private:
  PadMode fixed_ = PadMode::Notes;
  bool faders_ = false;
  bool song_slots_ = false;  // OS-SONGPADS: the Song screen's pads are SLOTS (false: TRACKS); main loop only, runtime
  bool shift_ = false;
  bool yielded_ = false;
  std::uint64_t taken_ = 0;  // pads (bit n = pad n) that the TRACKS layer took and that are still down
};

}  // namespace o3os::app
