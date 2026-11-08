// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-MTC A3 (docs/specs/OS-MTC.md 3.3): the main loop's answer to the time code generator's question. When a run starts or relocates anywhere but the song's first step, the sequencer (render)
// publishes the position it needs a song time for (Sequencer::mtc_request(): stamp, order position, pattern, step); this walks the song to it (seq/song_time.h, A1) and answers with a
// MtcAnchor command carrying the stamp and the time in whole samples. The render takes it only if the stamp is still its own, so a late answer is dropped, and it sends no time code until
// then (no wrong time is ever sent). Main loop only, once a pass: one relaxed word read and a compare; the walk (bounded by seq::kWalkCap) runs once per request, never per block. A position
// the song never plays (a capped answer) gets no answer: the generator stays silent until the next relocation.

#include <o3os/model/project.h>
#include <o3os/seq/sequencer.h>
#include <o3os/seq/song_time.h>

#include <cstdint>

namespace o3os::app {

class MtcSync {
 public:
  struct Stats {
    std::uint32_t answered = 0;  // MtcAnchor commands pushed
    std::uint32_t capped = 0;    // requests the map could not answer
    std::uint32_t retried = 0;   // answers the command ring had no room for (sent again on a later pass)
  };

  // `request` is Sequencer::mtc_request(). True when something was pushed this pass.
  bool step(const model::Project& project, std::uint32_t request, seq::CommandRing& ring) noexcept {
    const std::uint32_t stamp = request >> 24;
    if (stamp == 0u || stamp == done_) {
      return false;
    }
    const std::uint32_t order = (request >> 16) & 0xFFu;
    const std::uint32_t pattern = (request >> 8) & 0xFFu;
    const std::uint32_t step_in = request & 0xFFu;
    const bool song = order != 0xFFu;
    const seq::SongPoint p = song ? seq::song_time_at(project, seq::PlayMode::Song, order, step_in) : seq::song_time_at(project, seq::PlayMode::Pattern, pattern, step_in);
    if (p.capped) {
      ++stats_.capped;
      done_ = stamp;
      return false;
    }
    last_samples_ = core::q32_floor(p.time);
    if (!ring.push(seq::Command{seq::Command::Kind::MtcAnchor, static_cast<std::uint8_t>(stamp), 0, static_cast<std::uint32_t>(last_samples_)})) {
      ++stats_.retried;  // not done_: the next pass asks again
      return false;
    }
    done_ = stamp;
    ++stats_.answered;
    return true;
  }

  const Stats& stats() const noexcept { return stats_; }
  std::uint64_t last_samples() const noexcept { return last_samples_; }  // the last answer's song time, whole samples (the console's `mtc`)

 private:
  std::uint32_t done_ = 0;  // the last stamp handled
  std::uint64_t last_samples_ = 0;
  Stats stats_{};
};

// OS-MTC B3 (docs/specs/OS-MTC.md 4.3): the same, for the chase. MidiLink (render) asks which step of the song starts at or after the master's time (MidiLink::chase_request / chase_request_time: a stamp, the mode
// and pattern the unit would play, the song time asked in whole samples, MTC START already taken off); this walks the song with song_position_at (loop off: Q9 a, a time past the song's end is silence) and answers
// over the command ring with two commands, ChaseState (the tempo and swing before the step's own FX, and the swing pair's parity) and ChaseStart (the stamp, order << 7 | step, the step's song time in whole
// samples; pos 0xFFFF: past the end). A locate (a full frame while stopped) is answered the same way and moves the cursor only. The render takes an answer only for the stamp it waits for; a question this never
// answers (a walk the map cannot finish: the capped case) is asked again by the render after 500 ms. Main loop only, once a pass: two relaxed reads and a compare; the walk runs once per question, never per block.
class ChaseSync {
 public:
  struct Stats {
    std::uint32_t answered = 0;  // questions answered with a step
    std::uint32_t past_end = 0;  // questions answered "past the song's end"
    std::uint32_t capped = 0;    // questions the map could not answer
    std::uint32_t retried = 0;   // answers the command ring had no room for (sent again on a later pass)
  };

  // `word` is MidiLink::chase_request(), `song_samples` its chase_request_time(). True when something was pushed this pass.
  bool step(const model::Project& project, std::uint32_t word, std::uint32_t song_samples, seq::CommandRing& ring) noexcept {
    const std::uint32_t stamp = word >> 24;
    if (stamp == 0u || stamp == done_) {
      return false;
    }
    const bool song = ((word >> 16) & 1u) != 0u;
    const std::uint32_t pattern = (word >> 8) & 0xFFu;
    const core::Q32 t = static_cast<core::Q32>(song_samples) << core::kQ32Frac;
    const seq::SongPoint p = seq::song_position_at(project, song ? seq::PlayMode::Song : seq::PlayMode::Pattern, song ? 0u : pattern, t, false);
    if (p.capped) {
      ++stats_.capped;
      done_ = stamp;
      return false;
    }
    if (p.past_end) {
      if (!ring.push(seq::Command{seq::Command::Kind::ChaseStart, static_cast<std::uint8_t>(stamp), seq::Sequencer::kChasePastEnd, 0u})) {
        ++stats_.retried;
        return false;
      }
      ++stats_.past_end;
      done_ = stamp;
      return true;
    }
    const std::uint32_t order = song ? (p.order & 0xFFu) : 0u;
    if (!ring.push(seq::Command{seq::Command::Kind::ChaseState, static_cast<std::uint8_t>(p.odd ? 1u : 0u), static_cast<std::uint16_t>(p.swing_in), p.tempo_in_x100}) ||
        !ring.push(seq::Command{seq::Command::Kind::ChaseStart, static_cast<std::uint8_t>(stamp), static_cast<std::uint16_t>((order << 7) | (p.step & 0x7Fu)),
                                static_cast<std::uint32_t>(core::q32_floor(p.time))})) {
      ++stats_.retried;  // not done_: the next pass asks again (a ChaseState without its ChaseStart is harmless: the sequencer keeps it until a start arrives)
      return false;
    }
    ++stats_.answered;
    done_ = stamp;
    return true;
  }

  const Stats& stats() const noexcept { return stats_; }

 private:
  std::uint32_t done_ = 0;  // the last stamp handled
  Stats stats_{};
};

}  // namespace o3os::app
