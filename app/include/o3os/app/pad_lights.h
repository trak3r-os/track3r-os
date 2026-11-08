// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The pad LEDs of the OS-PADS layers (new Community OS behaviour). The pads are monochrome (pad_palette.h): Off, Back, Front.
//
//   TRACKS (pad_layer.h): 4 rows of 12 columns, the column is the track (0..11), pad 0 is the top left. LED grammar: Front = set by you, Back = silenced by someone else's solo (MUTE row),
//   blinking = pending, Off = sounding / inactive.
//     row 1  TRACK    Pattern: Front on the cursor's track; a track that fires a note flashes Back for a moment (TrackFlash). A press moves the cursor to the track.
//             AFFECTED  Perform: Front = the track is in the performance layer; a press joins or leaves it.
//     row 2  MUTE      Front = muted by you (the project's flag: sound level, obeys the track's MUTE MODE), Back = silenced by another track's solo, blinking = a quantised toggle is pending.
//     row 3  MUTE MODE Off = Immediate, Back = at the next bar, Front = at the end of the pattern (per track, runtime); a press cycles it.
//     row 4  SOLO      Front = soloed (immediate, any number of tracks).
//   The step popups (a held Instr, Fx or Note button): Instr lights the instruments that have content (a sample loaded, a module, a MIDI channel) Back and the current one Front;
//   Fx lights the current FX; Note lights nothing.

//   TrackFlash   the activity flash: the main loop's poll of the sequencer's note counters (ActivityWatch) turned into a short lit time per track.

#include <o3os/app/pad_maps.h>
#include <o3os/app/pad_palette.h>
#include <o3os/app/track_state.h>
#include <o3os/app/view_state.h>

#include <cstdint>

namespace o3os::app {

inline constexpr unsigned kPadCount = 48;
inline constexpr unsigned kPadCols = 12;

// The rows of the TRACKS layer, top to bottom.
enum class TracksRow : std::uint8_t { Track = 0, Mute = 1, MuteMode = 2, Solo = 3 };
constexpr TracksRow tracks_row(unsigned pad) noexcept { return static_cast<TracksRow>(pad / kPadCols); }
constexpr unsigned tracks_col(unsigned pad) noexcept { return pad % kPadCols; }

// How long a note's flash lasts, and the half period of a blink.
inline constexpr std::uint32_t kFlashMs = 120;
inline constexpr std::uint32_t kBlinkMs = 250;
constexpr bool blink_on(std::uint64_t now_ms) noexcept { return ((now_ms / kBlinkMs) & 1u) == 0u; }

class TrackFlash {
 public:
  // Once a pass. Lights the tracks that started a note since the last poll and puts out the ones whose time is up. True when the lit set changed. Without a sink nothing is lit.
  bool poll(const TrackSink* sink, std::uint64_t now_ms) noexcept {
    const std::uint32_t now = static_cast<std::uint32_t>(now_ms);
    std::uint16_t lit = lit_;
    const std::uint16_t fired = watch_.poll(sink);
    for (unsigned t = 0; t < kMixTracks; ++t) {
      if (((fired >> t) & 1u) != 0u) {
        at_[t] = now;
        lit = static_cast<std::uint16_t>(lit | (1u << t));
      } else if (((lit >> t) & 1u) != 0u && static_cast<std::uint32_t>(now - at_[t]) >= kFlashMs) {
        lit = static_cast<std::uint16_t>(lit & ~(1u << t));
      }
    }
    const bool changed = lit != lit_;
    lit_ = lit;
    return changed;
  }
  std::uint16_t lit() const noexcept { return lit_; }

 private:
  ActivityWatch watch_;
  std::uint16_t lit_ = 0;
  std::uint32_t at_[kMixTracks] = {};
};

// The TRACKS layer's 48 LEDs. `perform`: the Perform screen's (row 1 is AFFECTED). `flash`: TrackFlash::lit(). `blink`: the phase of a blink (blink_on).
void tracks_roles(const Model& model, const ViewState& view, bool perform, std::uint16_t flash, bool blink, PadRole (&roles)[48]) noexcept;

// Which of the patterns 1..48 hold something (the PATS pads' lit ones): a pattern with any step that is not empty. The main loop polls it once a pass while PATS is in force; a pattern is read
// again only when its generation moved (model::Project::pattern_generation) and at most two a pass, so the cost is bounded (2 * 16 tracks * 128 steps) however long the project is.
class PatternContent {
 public:
  static constexpr unsigned kPatterns = 48;
  void poll(const model::Project* project) noexcept;
  bool has(unsigned id) const noexcept { return id >= 1u && id <= kPatterns && ((mask_ >> (id - 1u)) & 1u) != 0u; }

 private:
  const model::Project* project_ = nullptr;
  std::uint64_t mask_ = 0;
  std::uint64_t seen_ = 0;
  std::uint32_t gen_[kPatterns] = {};
};

// The pads as patterns 1..48 (PATS; since OS-PATSWITCH also the held PATTERN chooser's): Back = the pattern holds a step, Front = the pattern that plays (stopped: the one in view), a queued one blinks (`blink`), Off = absent or empty.
void patterns_roles(const ViewState& view, const PatternContent& content, bool blink, PadRole (&roles)[48]) noexcept;

// OS-DJ A4: row 1 of the DJ page's pads (roles[0..11]) is the pattern row of the bank: a pattern that exists Back, the one that plays (stopped: the one in view) Front, the queued one blinking (`blink`: the phase), a
// pattern the project lacks or past 255 Off.
void dj_pattern_roles(const Model& model, const ViewState& view, bool blink, PadRole (&roles)[48]) noexcept;

// The LEDs of a fixed PIANO / ISO / ... mode (pad_layer.h): view.padmode.fixed() says which. A note pad: the root Front, the other notes of the scale Back, a note outside the scale
// and a pad with no note Off; the pattern jumps and the octave pads Back (the octave ones Off at the end of 0..9). STEP: step_roles. PATS: Back = has content, Front = playing (stopped:
// the pattern being edited), blinking Front / Off = queued (`blink`: the phase, blink_on). NOTES (OS-NOTESLED): the board's own notes (view.edit.pads) lit the same way, the root Front,
// the others of the scale Back, a note outside the scale Off; with SCALE LOCK the board holds scale notes only, so only the roots stand out. VU: column c is track c, a 4-row bar from the bottom of the track's published peak (the board's
// MeterState on the DbScale: a quarter of the scale a row), the top lit pad Front; FADERS: the same pads lit to the track's volume (a row is 25 %).
void mode_roles(const Model& model, const ViewState& view, const PatternContent& content, bool blink, PadRole (&roles)[48]) noexcept;

// The LEDs while a step popup is open (view.popup.kind says which).
void popup_roles(const Model& model, const ViewState& view, PadRole (&roles)[48]) noexcept;

}  // namespace o3os::app
