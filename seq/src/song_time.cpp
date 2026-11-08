// SPDX-License-Identifier: GPL-3.0-or-later

// OS-MTC slice A1: the song time map, the walk (docs/specs/OS-MTC.md section 3.2; the header song_time.h has the contract). Main-loop
// context, pure: the only inputs are the project and the target, and the only arithmetic is the sequencer's own (see the header), so the
// differential test against an offline Sequencer run (tests/seq_song_time_test.cpp) is the proof that the two agree.

#include <o3os/seq/song_time.h>

#include <o3os/seq/rows.h>

#include <algorithm>
#include <cstdint>

namespace o3os::seq {

namespace {

constexpr std::uint32_t clamp_u32(std::uint32_t v, std::uint32_t lo, std::uint32_t hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }
constexpr std::int32_t clamp_i32(std::int32_t v, std::int32_t lo, std::int32_t hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

// Sequencer::song_pattern_len, as the sequencer computes it.
std::uint32_t song_pattern_len(const model::Project& p, std::uint32_t pattern_id) noexcept {
  std::uint32_t len = p.pattern_length(pattern_id);  // the longest of the active tracks; 0 when the pattern does not exist
  if (len == 0u) {
    len = p.default_pattern_length();  // a slot naming a missing pattern plays as empty, at the default length
  }
  return clamp_u32(len, model::kMinPatternLength, model::kMaxPatternLength);
}

// The walk: the sequencer's run-start state and cursors, walked one step at a time. The performance layer, the external clock, the step
// rate, live jumps, queued patterns, locates and switches are not in it (A-4): a walk is a run from the song start on the internal clock.
struct Walk {
  const model::Project& p;
  PlayMode mode;
  // a run's start state (begin_run with resuming = false): no FX tempo, no Swing FX, the project tempo, the pair parity at its first step
  std::uint32_t project_tempo_x100 = 0;
  std::uint32_t fx_tempo_x100 = 0;
  std::uint8_t swing_fx = 0;
  std::uint8_t project_swing = 0;
  bool swing_odd = false;
  core::Q32 time = 0;  // the current step's start, from the song start
  // the song cursor
  std::uint32_t order_pos = 0;
  std::uint32_t pattern_id = 0;
  std::uint32_t pattern_len = 1;
  std::uint32_t song_step = 0;
  // the tracks' cursors
  std::uint8_t trk_pos[model::kMaxTracks] = {};
  std::uint8_t trk_len[model::kMaxTracks] = {};
  // the row FX of the step being walked
  bool break_pending = false;
  bool jump_pending = false;
  std::uint32_t jump_order = 0;
  std::int32_t tempo_fine_row = 0;
  std::uint32_t walked = 0;
  bool wrapped = false;  // the song's loop back to its first slot happened (the order list ended): the chase's walk stops there

  std::uint32_t active() const noexcept { return std::min<std::uint32_t>(p.active_tracks(), model::kMaxTracks); }
  // Sequencer::nominal_tempo_x100 without the live layers (no performance override, no external clock): the FX tempo, else the project tempo.
  std::uint32_t nominal_tempo_x100() const noexcept {
    return clamp_u32(fx_tempo_x100 != 0u ? fx_tempo_x100 : project_tempo_x100, model::kMinTempoX100, model::kMaxTempoX100);
  }
  // Sequencer::swing without the ParamChange.
  std::uint32_t swing() const noexcept {
    return clamp_u32(swing_fx != 0u ? swing_fx : project_swing, model::kMinSwing, model::kMaxSwing);
  }
  // Sequencer::load_tracks (forward play mode, no performance length).
  void load_tracks(std::uint32_t from_step) noexcept {
    for (std::uint32_t t = 0; t < model::kMaxTracks; ++t) {
      const std::uint32_t len = track_length(p, pattern_id, t, pattern_len);  // seq/rows.h: the track's length in this pattern, clamped 1..128
      trk_len[t] = static_cast<std::uint8_t>(len);
      trk_pos[t] = static_cast<std::uint8_t>(from_step % len);
    }
  }
  // Sequencer::pattern_end without the live layers (no queued pattern, no live jump, no mode request; the song loops as Config::song_loop defaults).
  void pattern_end() noexcept {
    break_pending = false;
    bool order_set = false;
    if (jump_pending) {
      jump_pending = false;
      if (mode == PlayMode::Song && jump_order < p.song_length()) {
        order_pos = jump_order;  // Order jump (core FX 45): the song carries on at that position
        order_set = true;
      }
      // out of range, or no song to jump in: it ends the pattern like a Break pattern
    }
    if (!order_set && mode == PlayMode::Song) {
      ++order_pos;
      if (order_pos >= p.song_length()) {
        order_pos = 0;  // the song's loop back to its first slot
        wrapped = true;
      }
    }
    if (mode == PlayMode::Song) {
      pattern_id = p.order(order_pos);  // 0 past the list's end: an empty pattern
    }
    pattern_len = song_pattern_len(p, pattern_id);
    song_step = 0;
    load_tracks(0);
  }
  // Sequencer::end_step without a pending locate or switch.
  void advance() noexcept {
    const std::uint32_t act = active();
    for (std::uint32_t t = 0; t < act; ++t) {  // Sequencer::advance_tracks, forward play mode
      if (++trk_pos[t] >= trk_len[t]) {
        trk_pos[t] = 0;
      }
    }
    ++song_step;
    if (song_step >= pattern_len || break_pending || jump_pending) {
      pattern_end();
    }
  }
  // One step of the walk: the rows and their timing FX (read_row + pre_fx), then the step's length (finish_step), the point at its start, and the advance.
  SongPoint step() noexcept {
    // The rows of this step: every active track reads its row, sanitises the FX (read_row), and pre_fx runs the columns in their order (1, then 0).
    tempo_fine_row = 0;
    break_pending = false;
    jump_pending = false;
    const std::uint32_t tempo_in = nominal_tempo_x100();  // OS-MTC B3: the state before this row's FX
    const std::uint32_t swing_in = swing();
    const bool odd_in = swing_odd;
    const std::uint32_t act = active();
    for (std::uint32_t t = 0; t < act; ++t) {
      model::Step row;
      (void)read_step(p, pattern_id, t, trk_pos[t], row);  // NoPattern or Torn: the empty row
      std::uint8_t type[2] = {row.fx[0].type, row.fx[1].type};
      std::uint8_t value[2] = {row.fx[0].value, row.fx[1].value};
      fx_sanitize(type[0], value[0]);
      fx_sanitize(type[1], value[1]);
      for (int col = 1; col >= 0; --col) {
        if (!map_fx_known(type[col])) {
          continue;  // not a Row FX of the table: inert for the walk
        }
        switch (type[col]) {
          case fx::kTempo:
            // PAR-082: the Tempo FX is value x 2 BPM, values below 40 BPM are 40
            fx_tempo_x100 = clamp_u32(static_cast<std::uint32_t>(value[col]) * 2u, kMinTempoFxBpm, 800u) * 100u;
            break;
          case fx::kSwing:
            swing_fx = value[col];  // from the row it is in until another one or the stop
            break;
          case fx::kTempoFine:
            tempo_fine_row += (static_cast<std::int32_t>(value[col]) - 128) * 25;  // (v - 128) / 4 BPM, in BPM x 100
            break;
          case fx::kBreakPattern:
            break_pending = true;
            break;
          case fx::kOrderJump:
            jump_pending = true;
            jump_order = value[col];
            break;
          default:
            break;  // a known Row FX that does not move the timeline
        }
      }
    }
    // finish_step: the Tempo fine of the row is folded into the tempo in force (and stays, as the FX tempo does); the step's length follows
    // the tempo and swing the rows left, so the step that holds the FX already uses it.
    if (tempo_fine_row != 0) {
      fx_tempo_x100 = static_cast<std::uint32_t>(
          clamp_i32(static_cast<std::int32_t>(nominal_tempo_x100()) + tempo_fine_row, static_cast<std::int32_t>(model::kMinTempoX100),
                    static_cast<std::int32_t>(model::kMaxTempoX100)));
    }
    const std::uint32_t tempo = nominal_tempo_x100();
    const std::uint32_t s = swing();
    const core::Q32 dur = step_length_q32(core::samples_per_tick_q32(tempo), s, swing_odd);
    swing_odd = !swing_odd;
    SongPoint pt{};
    pt.time = time;
    pt.tempo_x100 = tempo;
    pt.swing = static_cast<std::uint8_t>(s);
    pt.order = mode == PlayMode::Song ? static_cast<std::uint16_t>(order_pos) : audio::PlayPosition::kNoOrder;
    pt.pattern = static_cast<std::uint8_t>(pattern_id);
    pt.step = static_cast<std::uint16_t>(song_step);
    pt.tempo_in_x100 = tempo_in;
    pt.swing_in = static_cast<std::uint8_t>(swing_in);
    pt.odd = odd_in;
    pt.past_end = false;
    pt.capped = false;
    time += dur;
    advance();
    return pt;
  }
};

Walk walk_init(const model::Project& p, PlayMode mode, std::uint32_t pattern_id) noexcept {
  Walk w{p, mode};
  w.project_tempo_x100 = clamp_u32(p.tempo_x100(), model::kMinTempoX100, model::kMaxTempoX100);  // tempo_sm_.set(clamp(...)) at a run's start
  w.project_swing = p.swing();
  if (mode == PlayMode::Song) {
    w.order_pos = 0;
    w.pattern_id = p.order(0);
  } else {
    w.order_pos = audio::PlayPosition::kNoOrder;
    w.pattern_id = pattern_id;
  }
  w.pattern_len = song_pattern_len(p, w.pattern_id);
  w.song_step = 0;
  w.load_tracks(0);
  return w;
}

}  // namespace

SongPoint song_time_at(const model::Project& project, PlayMode mode, std::uint32_t order, std::uint32_t step) noexcept {
  const bool song = mode == PlayMode::Song;
  // The target's pattern and its length, known up front: Song mode names the slot's pattern, Pattern mode the id itself (a pattern
  // id in 1..255 that does not exist plays as empty at the default length, so it is reachable and walked).
  const std::uint32_t target_pattern = song ? project.order(order) : order;
  const std::uint32_t target_len = song_pattern_len(project, target_pattern);
  // A target the song never plays answers at once, without walking kWalkCap steps of rows: Song mode's orders are 0..song_length-1
  // (the list loops; an empty list still plays its position 0 as an empty pattern, so the bound is max(song_length, 1)), Pattern
  // mode's pattern id must exist, and a step is 0..len-1 of the target pattern (a segment's step always is; a from_step is wrapped
  // by the caller, as begin_run wraps it). The answer echoes the request and is `capped`: not a match.
  if ((song && order >= std::max<std::uint32_t>(project.song_length(), 1u)) || (!song && project.pattern(order) == nullptr) ||
      step >= target_len) {
    SongPoint none{};
    none.capped = true;
    none.order = song ? static_cast<std::uint16_t>(order & 0xFFFFu) : audio::PlayPosition::kNoOrder;
    none.pattern = static_cast<std::uint8_t>(target_pattern & 0xFFu);
    none.step = static_cast<std::uint16_t>(step & 0xFFFFu);
    return none;
  }
  Walk w = walk_init(project, mode, song ? 0u : order);  // Pattern mode: `order` carries the pattern id
  for (;;) {
    const SongPoint pt = w.step();
    ++w.walked;
    if (song ? (pt.order == order && pt.step == step) : (pt.step == step)) {
      return pt;
    }
    if (w.walked >= kWalkCap) {
      SongPoint cap = pt;
      cap.capped = true;
      return cap;
    }
  }
}

SongPoint song_position_at(const model::Project& project, PlayMode mode, std::uint32_t order, core::Q32 samples, bool loop) noexcept {
  Walk w = walk_init(project, mode, mode == PlayMode::Song ? 0u : order);
  for (;;) {
    const SongPoint pt = w.step();
    ++w.walked;
    if (pt.time >= samples) {
      return pt;
    }
    if (!loop && w.wrapped) {
      SongPoint end = pt;  // the end of the song came before the time: it is past the last step (this one lasts until the end)
      end.past_end = true;
      return end;
    }
    if (w.walked >= kWalkCap) {
      SongPoint cap = pt;
      cap.capped = true;
      return cap;
    }
  }
}

}  // namespace o3os::seq
