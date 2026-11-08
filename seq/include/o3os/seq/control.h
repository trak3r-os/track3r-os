// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The control layers: who controls a voice parameter on a track right now (OS-005c; ADR 0013 D5 "the three control layers (sequencer, sequencer 2,
// performance; BASE-20) are a per-parameter source table, pure and host-tested"; triage BASE-20).
//
// BEHAVIOUR. Each track's voice
// has three controllers per parameter: the sequencer (the FX in the step's FIRST FX column, the more significant), the sequencer's second layer
// (the SECOND column) and the performance mode (the live Perform screen's FX slots). A column keeps ONE FX at a time: a note step replaces both
// columns' FX (a column without one ends its previous FX), a step without a note replaces a column's FX only when
// the column holds a voice FX or Off, and an empty column of such a step changes nothing. Between layers the rule is: performance over the first column over
// the second; when a less significant effect ends while a more significant one is active, nothing changes but the bookkeeping; when the more significant one ends and
// the less significant is active, it takes over. The performance layer works on the value the sequencer layers made
// (the sequencer values are kept even when performance is active, because performance is computed on them), so a winner that is performance carries
// both.
//
// WHAT THIS IS. The same rule as a table, pure and testable: for each track, the FX each column holds (type and value), and for each group (seq/fx.h) the
// performance layer's value. A "parameter" here is a GROUP: the set of FX types that drive one voice parameter (the filter FX 27, 28, 29 are one parameter, not
// three). resolve() gives the winner of a group on a track: its layer, the sequencer layer's FX type and value (the one of the more significant column that
// holds the group, or else the other), and the performance value. A change of the winner is what the sequencer emits as a ModSet (a new or changed winner) or
// ModEnd (no source left) event; a change that leaves the winner as it was (the second column's value under an active first column) emits nothing, which is
// "nothing changes but the bookkeeping". The voices (OS-005d) see only these events, never the arbitration.
//
// Perform (the P2 screen) drives the performance layer through Sequencer::set_perf and end_perf; their values are carried as int16 (the performance
// values range from 8 to 32 bits: start and end points are sample positions, which the P2 contract will normalise: open item, default int16 as given).
//
// State: per track two bytes of type, two of value and a word of performance bits: 16 x 8 = 128 bytes, in the table. The performance values (an int16 per track and group, 16 x 25 x 2 = 800 bytes)
// are the caller's PerfStore, attached with attach_perf(): on the target it is in SDRAM (SRAM_U keeps its 32 KiB free floor; a value is read once an FX event and written once a Perform
// gesture, never in the sample loop), inside the Sequencer's OrderStore. A table without a store has no performance layer (set_perf does nothing, resolve reports no performance value).

#include <o3os/audio/events.h>
#include <o3os/seq/fx.h>

#include <cstdint>

namespace o3os::seq {

struct ModState {
  bool active;
  std::uint8_t layer;    // the winner: audio::kLayerFx1, kLayerFx2 or kLayerPerf
  std::uint8_t fx_type;  // the sequencer layers' winning FX (0 when only the performance layer holds the group)
  std::uint8_t value;    // its value
  std::int16_t perf;     // the performance layer's value (0 when it does not hold the group)
};
constexpr bool operator==(const ModState& a, const ModState& b) noexcept {
  return a.active == b.active && a.layer == b.layer && a.fx_type == b.fx_type && a.value == b.value && a.perf == b.perf;
}
constexpr bool operator!=(const ModState& a, const ModState& b) noexcept { return !(a == b); }

struct ModChange {
  std::uint8_t track;
  FxGroup group;
  ModState now;  // active false: the group has no source left (ModEnd)
};

class ControlTable {
 public:
  static constexpr std::uint32_t kTracks = 16;
  static constexpr std::uint32_t kColumns = 2;

  // The performance layer's values: one int16 per track and group (the bits saying which are held stay in the table).
  struct PerfStore {
    std::int16_t val[kTracks][kFxGroupCount];
  };
  // Gives the table its performance store (zeroed here, and the performance bits cleared), before the first set_perf.
  void attach_perf(PerfStore& store) noexcept;

  void reset() noexcept;
  // The two sequencer columns of every track end and the performance layer stays (a fresh transport start, OS-011b): a play ends the sequencer's FX
  // only and leaves the performance mode's, which the Perform screen owns until it lets them go.
  void reset_columns() noexcept;

  // FX column `column` (0 or 1) of `track` now holds the Voice-kind FX (type, value); type 0 (or any type that is not a voice FX) means the column holds none
  // (its previous FX ends). Returns how many groups changed their winner (0..2), written to out[0..1].
  std::uint32_t set_column(std::uint8_t track, std::uint8_t column, std::uint8_t type, std::uint8_t value, ModChange out[2]) noexcept;
  // Several columns at once (a note step replaces both): `mask` has a bit for each column that is set (bit 0 the first column), type[] and value[] hold what they
  // now hold. The changes are the NET changes of the groups the columns touched, up to four (a transient state between the two columns, a filter that one column
  // ends while the other replaces it, makes no event).
  std::uint32_t set_columns(std::uint8_t track, std::uint8_t mask, const std::uint8_t type[2], const std::uint8_t value[2], ModChange out[4]) noexcept;
  // The column's FX ends (set_column with type 0).
  std::uint32_t end_column(std::uint8_t track, std::uint8_t column, ModChange out[2]) noexcept { return set_column(track, column, 0, 0, out); }
  // The performance layer takes the group on the track (or changes its value), or lets it go. Returns 0 or 1 (a change written to out[0]).
  std::uint32_t set_perf(std::uint8_t track, FxGroup g, std::int16_t value, ModChange out[1]) noexcept;
  std::uint32_t end_perf(std::uint8_t track, FxGroup g, ModChange out[1]) noexcept;
  // Every column and every performance entry of a track (or of all) ends: the changes are not reported (the caller sends an FxReset or ModEnds).
  void clear_track(std::uint8_t track) noexcept;

  ModState resolve(std::uint8_t track, FxGroup g) const noexcept;
  std::uint8_t column_type(std::uint8_t track, std::uint8_t column) const noexcept;
  std::uint8_t column_value(std::uint8_t track, std::uint8_t column) const noexcept;
  bool perf_holds(std::uint8_t track, FxGroup g) const noexcept;
  // The groups of a track that have a source: a bit per group.
  std::uint32_t active_groups(std::uint8_t track) const noexcept;

 private:
  std::uint8_t type_[kTracks][kColumns] = {};
  std::uint8_t value_[kTracks][kColumns] = {};
  std::uint32_t perf_on_[kTracks] = {};
  PerfStore* perf_ = nullptr;
};

}  // namespace o3os::seq
