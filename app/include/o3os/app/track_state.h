// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the app knows about a track's mute, solo and activity, for the TRACKS pads (OS-MUTE, new Community OS behaviour; the pad grammar is OS-PADS').
//
//   MUTE MODE      per track (the project's since OS-PERSIST2: the MUTE chunk): Immediate (the default), Bar, Pattern. It says when a MUTE toggle of that track takes effect: at once, at the next
//                  bar (16 steps of the pattern from its start) or at the end of the pattern. SOLO is always immediate. track.mute_mode cycles it.
//   TrackSink      the sequencer's side, wired by the platform (and by a test): the deferred toggle's command, the pending bits, the note counters. Main loop only.
//   track_lamp()   one track's state, read from the project (mute, solo: the Track parameters), the mixer board (CUE route, mode) and the sink (pending).
//   ActivityWatch  the main loop's copy of the note counters: poll() says which tracks fired a note since the last poll (a short LED flash).

#include <o3os/app/mix_board.h>
#include <o3os/app/model.h>
#include <o3os/audio/graph_params.h>

#include <cstdint>

namespace o3os::app {

enum class MuteMode : std::uint8_t { Immediate = 0, Bar = 1, Pattern = 2 };
inline constexpr unsigned kMuteModes = 3;
inline constexpr const char* kMuteModeNames[kMuteModes] = {"IMMEDIATE", "BAR", "PATTERN"};

constexpr MuteMode next_mute_mode(MuteMode m) noexcept { return static_cast<MuteMode>((static_cast<unsigned>(m) + 1u) % kMuteModes); }

class TrackSink {
 public:
  // The transport is running (a deferred toggle has a boundary to wait for).
  virtual bool playing() const noexcept = 0;
  // The project's mute flag of `track` has just changed; its effective mute (the strip, the MIDI notes) stays `was_muted` until the boundary of `mode` (Bar or Pattern). False: not accepted
  // (no sequencer, the ring is full): the caller tells the graph at once.
  virtual bool mute_defer(unsigned track, MuteMode mode, bool was_muted) noexcept = 0;
  // The deferred toggle of `track` is withdrawn (the flag has been put back). False: not sent.
  virtual bool mute_cancel(unsigned track) noexcept = 0;
  // Bit t set: a toggle of track t waits for its boundary.
  virtual std::uint16_t mute_pending() const noexcept = 0;
  // The notes track t has started, modulo 256 (Sequencer::note_count).
  virtual std::uint8_t note_count(unsigned track) const noexcept = 0;
  // OS-PARAMAUTO S2 (Sequencer::set_auto_mask / take_auto): the letters CC A..F that move a parameter, and the values the sequencer has published for them. Not pure: a sink without a sequencer
  // maps nothing and takes nothing.
  virtual void auto_mask(std::uint8_t) noexcept {}
  virtual std::uint8_t auto_take(std::uint8_t*) noexcept { return 0; }

 protected:
  TrackSink() = default;
  ~TrackSink() = default;
};

struct TrackLamp {
  bool muted = false;          // the project's mute flag: what the user set (the Track parameter Mute)
  bool soloed = false;         // the Track parameter Solo
  bool solo_silenced = false;  // another active track is soloed and this one is not: silent without being muted
  bool mute_pending = false;   // a toggle waits for its boundary (the pad blinks)
  bool cue = false;            // the track's CUE route bit (an extra send to the CUE bus; MAIN stays)
  MuteMode mode = MuteMode::Immediate;
};

// The state of track `t` (0..15). A track outside the active ones, or a model without a project, reads as plain. Main loop only.
inline TrackLamp track_lamp(const Model& model, unsigned t) noexcept {
  TrackLamp l;
  if (t >= kMixTracks) {
    return l;
  }
  if (model.mix != nullptr) {
    l.cue = (model.mix->route[t] & audio::route::kCue) != 0u;
    l.mode = static_cast<MuteMode>(model.mix->mute_mode[t] < kMuteModes ? model.mix->mute_mode[t] : 0u);
    l.mute_pending = model.mix->tracks != nullptr && ((model.mix->tracks->mute_pending() >> t) & 1u) != 0u;
  }
  const model::Project* p = model.project;
  if (p == nullptr || t >= p->active_tracks()) {
    return l;
  }
  const model::TrackSlot* s = p->track(t);
  l.muted = s != nullptr && (s->flags & model::kTrackMute) != 0u;
  l.soloed = s != nullptr && (s->flags & model::kTrackSolo) != 0u;
  bool any_solo = false;
  for (unsigned i = 0; i < p->active_tracks() && i < kMixTracks; ++i) {
    const model::TrackSlot* o = p->track(i);
    any_solo = any_solo || (o != nullptr && (o->flags & model::kTrackSolo) != 0u);
  }
  l.solo_silenced = any_solo && !l.soloed;
  return l;
}

// The main loop's copy of the sequencer's note counters. The first poll only takes the counters (nothing flashes at start-up).
class ActivityWatch {
 public:
  // Bit t set: track t started a note since the last poll. Without a sink, 0.
  std::uint16_t poll(const TrackSink* sink) noexcept {
    std::uint16_t changed = 0;
    for (unsigned t = 0; sink != nullptr && t < kMixTracks; ++t) {
      const std::uint8_t now = sink->note_count(t);
      changed = static_cast<std::uint16_t>(changed | ((primed_ && now != last_[t]) ? 1u << t : 0u));
      last_[t] = now;
    }
    primed_ = primed_ || sink != nullptr;
    return changed;
  }

 private:
  std::uint8_t last_[kMixTracks] = {};
  bool primed_ = false;
};

}  // namespace o3os::app
