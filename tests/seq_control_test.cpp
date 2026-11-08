// SPDX-License-Identifier: GPL-3.0-or-later
//
// The control layers (seq/control.h, OS-005c): the arbitration (performance over the first FX column over the second; a more significant FX that ends hands the
// parameter to the less significant one; a less significant one changing under an active more significant one changes nothing) against a brute-force model, over
// random sequences, plus hand-written cases. Host and Cortex-M4F (`qemu_seq`).
#include <doctest/doctest.h>

#include <o3os/seq/control.h>

#include <cstdint>
#include <initializer_list>

#include "support/rng.h"

namespace seq = o3os::seq;
namespace audio = o3os::audio;
using seq::FxGroup;
using o3os::test::Rng;

namespace {

// The model: what each column and the performance layer hold, and the winner of a group worked out from scratch.
struct Model {
  std::uint8_t type[16][2] = {};
  std::uint8_t value[16][2] = {};
  bool perf_on[16][32] = {};
  std::int16_t perf_val[16][32] = {};

  seq::ModState resolve(std::uint8_t t, FxGroup g) const {
    seq::ModState s{false, 0, 0, 0, 0};
    for (std::uint32_t c = 0; c < 2; ++c) {
      if (type[t][c] != 0 && seq::fx_group(type[t][c], c) == g) {
        s.active = true;
        s.layer = static_cast<std::uint8_t>(c);
        s.fx_type = type[t][c];
        s.value = value[t][c];
        break;
      }
    }
    if (perf_on[t][static_cast<std::uint32_t>(g)]) {
      s.active = true;
      s.layer = audio::kLayerPerf;
      s.perf = perf_val[t][static_cast<std::uint32_t>(g)];
    }
    return s;
  }
};

std::uint8_t pick_type(Rng& rng) {
  // voice FX, a few of each group, engine FX, inert and row types
  static const std::uint8_t types[] = {19, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 41, 42, 18, 7, 130, 140, 3, 2, 0, 14};
  return types[rng.below(sizeof types)];
}

}  // namespace

TEST_CASE("control layers: hand-written cases") {
  seq::ControlTable ct;
  seq::ControlTable::PerfStore ct_perf;
  ct.attach_perf(ct_perf);
  ct.reset();
  seq::ModChange ch[2];
  // Panning in the second column: it controls the parameter.
  CHECK(ct.set_column(0, 1, seq::fx::kPanning, 30, ch) == 1u);
  CHECK(ch[0].group == FxGroup::Panning);
  CHECK(ch[0].now.active);
  CHECK(ch[0].now.layer == audio::kLayerFx2);
  CHECK(ch[0].now.value == 30);
  // Panning in the first column too: the more significant one wins, the value changes.
  CHECK(ct.set_column(0, 0, seq::fx::kPanning, 80, ch) == 1u);
  CHECK(ch[0].now.layer == audio::kLayerFx1);
  CHECK(ch[0].now.value == 80);
  // The less significant one changes its value under the active more significant one: nothing happens but its own record.
  CHECK(ct.set_column(0, 1, seq::fx::kPanning, 10, ch) == 0u);
  CHECK(ct.resolve(0, FxGroup::Panning).value == 80);
  // The more significant one ends: the less significant takes over, with ITS value.
  CHECK(ct.end_column(0, 0, ch) == 1u);
  CHECK(ch[0].now.active);
  CHECK(ch[0].now.layer == audio::kLayerFx2);
  CHECK(ch[0].now.value == 10);
  // The less significant ends: nothing is left.
  CHECK(ct.end_column(0, 1, ch) == 1u);
  CHECK_FALSE(ch[0].now.active);
  CHECK_FALSE(ct.resolve(0, FxGroup::Panning).active);
  // A column that holds no FX and is told to end changes nothing.
  CHECK(ct.end_column(0, 1, ch) == 0u);
}

TEST_CASE("control layers: performance over the sequencer layers, which stay underneath") {
  seq::ControlTable ct;
  seq::ControlTable::PerfStore ct_perf;
  ct.attach_perf(ct_perf);
  ct.reset();
  seq::ModChange ch[2];
  CHECK(ct.set_column(3, 0, seq::fx::kLowPass, 40, ch) == 1u);
  CHECK(ch[0].group == FxGroup::Filter);
  // another filter type in the other column: the same parameter, the first column wins, no change
  CHECK(ct.set_column(3, 1, seq::fx::kHighPass, 70, ch) == 0u);
  // the performance layer takes it: it wins and carries both its value and the sequencer layer's
  CHECK(ct.set_perf(3, FxGroup::Filter, -12, ch) == 1u);
  CHECK(ch[0].now.layer == audio::kLayerPerf);
  CHECK(ch[0].now.perf == -12);
  CHECK(ch[0].now.fx_type == seq::fx::kLowPass);
  CHECK(ch[0].now.value == 40);
  // a different value of the same performance entry is a change
  CHECK(ct.set_perf(3, FxGroup::Filter, 5, ch) == 1u);
  CHECK(ch[0].now.perf == 5);
  CHECK(ct.set_perf(3, FxGroup::Filter, 5, ch) == 0u);  // the same again is not
  // the sequencer layer ends under it: the winner is still the performance layer, with the other column's FX beneath
  CHECK(ct.end_column(3, 0, ch) == 1u);
  CHECK(ch[0].now.layer == audio::kLayerPerf);
  CHECK(ch[0].now.fx_type == seq::fx::kHighPass);
  // the performance layer lets go: the sequencer's FX is what is left
  CHECK(ct.end_perf(3, FxGroup::Filter, ch) == 1u);
  CHECK(ch[0].now.layer == audio::kLayerFx2);
  CHECK(ct.end_perf(3, FxGroup::Filter, ch) == 0u);
  // other tracks and groups are independent
  CHECK_FALSE(ct.resolve(2, FxGroup::Filter).active);
  CHECK_FALSE(ct.resolve(3, FxGroup::Panning).active);
}

// OS-PERFFX P1: the Overdrive and Bit depth groups (the Perform screen's OVERDRV and BITDEPTH) are two more groups of the same arbitration. The layer carries its own value and the step FX
// underneath (the voices keep the layer's value and tell the step layer what is under it); when the layer ends, the winner is the step FX again, or nothing.
TEST_CASE("control layers: the Overdrive and Bit depth groups - the Perform layer over a step FX, and its end sends the step FX again") {
  seq::ControlTable ct;
  seq::ControlTable::PerfStore ct_perf;
  ct.attach_perf(ct_perf);
  ct.reset();
  seq::ModChange ch[2];
  struct Case {
    FxGroup group;
    std::uint8_t fx;
    std::uint8_t step_value;
    std::int16_t perf_value;
  };
  for (const Case c : {Case{FxGroup::Overdrive, seq::fx::kOverdrive, 30, 55}, Case{FxGroup::BitDepth, seq::fx::kBitDepth, 8, 5}}) {
    // the layer alone: no FX underneath
    CHECK(ct.set_perf(2, c.group, c.perf_value, ch) == 1u);
    CHECK(ch[0].group == c.group);
    CHECK(ch[0].now.active);
    CHECK(ch[0].now.layer == audio::kLayerPerf);
    CHECK(ch[0].now.perf == c.perf_value);
    CHECK(ch[0].now.fx_type == 0);
    // a step FX comes in under it: still the layer's, now with the FX in a and b
    CHECK(ct.set_column(2, 0, c.fx, c.step_value, ch) == 1u);
    CHECK(ch[0].now.layer == audio::kLayerPerf);
    CHECK(ch[0].now.perf == c.perf_value);
    CHECK(ch[0].now.fx_type == c.fx);
    CHECK(ch[0].now.value == c.step_value);
    // another value of the layer
    CHECK(ct.set_perf(2, c.group, static_cast<std::int16_t>(c.perf_value + 1), ch) == 1u);
    CHECK(ch[0].now.perf == c.perf_value + 1);
    // the layer ends: the step FX is what the voices are told, as a ModSet of the column's layer
    CHECK(ct.end_perf(2, c.group, ch) == 1u);
    CHECK(ch[0].now.active);
    CHECK(ch[0].now.layer == audio::kLayerFx1);
    CHECK(ch[0].now.fx_type == c.fx);
    CHECK(ch[0].now.value == c.step_value);
    // with no step FX the end is the group's end
    CHECK(ct.set_perf(2, c.group, c.perf_value, ch) == 1u);
    CHECK(ct.end_column(2, 0, ch) == 1u);
    CHECK(ch[0].now.layer == audio::kLayerPerf);
    CHECK(ch[0].now.fx_type == 0);
    CHECK(ct.end_perf(2, c.group, ch) == 1u);
    CHECK_FALSE(ch[0].now.active);
    CHECK_FALSE(ct.perf_holds(2, c.group));
    // another track is not touched
    CHECK_FALSE(ct.resolve(3, c.group).active);
  }
  // the two groups are independent
  CHECK(ct.set_perf(0, FxGroup::Overdrive, 40, ch) == 1u);
  CHECK_FALSE(ct.perf_holds(0, FxGroup::BitDepth));
  CHECK(ct.set_perf(0, FxGroup::BitDepth, 3, ch) == 1u);
  CHECK(ct.end_perf(0, FxGroup::Overdrive, ch) == 1u);
  CHECK(ct.perf_holds(0, FxGroup::BitDepth));
}

TEST_CASE("control layers: a new FX in a column replaces the old one and may move the column to another group; engine FX do not compete") {
  seq::ControlTable ct;
  seq::ControlTable::PerfStore ct_perf;
  ct.attach_perf(ct_perf);
  ct.reset();
  seq::ModChange ch[2];
  CHECK(ct.set_column(1, 0, seq::fx::kPanning, 50, ch) == 1u);
  // the same column now holds a filter: panning ends, filter starts: two changes
  CHECK(ct.set_column(1, 0, seq::fx::kLowPass, 20, ch) == 2u);
  bool saw_end = false, saw_set = false;
  for (int i = 0; i < 2; ++i) {
    if (ch[i].group == FxGroup::Panning && !ch[i].now.active) {
      saw_end = true;
    }
    if (ch[i].group == FxGroup::Filter && ch[i].now.active) {
      saw_set = true;
    }
  }
  CHECK(saw_end);
  CHECK(saw_set);
  // a row FX or None in the column ends what it held and holds nothing
  CHECK(ct.set_column(1, 0, seq::fx::kRoll, 5, ch) == 1u);
  CHECK_FALSE(ct.resolve(1, FxGroup::Filter).active);
  CHECK(ct.column_type(1, 0) == 0);
  // engine FX in both columns: two groups, both active
  CHECK(ct.set_column(2, 0, 130, 7, ch) == 1u);
  CHECK(ct.set_column(2, 1, 130, 9, ch) == 1u);
  CHECK(ct.resolve(2, FxGroup::Engine0).value == 7);
  CHECK(ct.resolve(2, FxGroup::Engine1).value == 9);
  CHECK(ct.active_groups(2) == ((1u << static_cast<std::uint32_t>(FxGroup::Engine0)) | (1u << static_cast<std::uint32_t>(FxGroup::Engine1))));
  ct.clear_track(2);
  CHECK(ct.active_groups(2) == 0u);
  // out-of-range arguments change nothing
  CHECK(ct.set_column(16, 0, seq::fx::kPanning, 1, ch) == 0u);
  CHECK(ct.set_column(0, 2, seq::fx::kPanning, 1, ch) == 0u);
  CHECK(ct.set_perf(0, FxGroup::None, 1, ch) == 0u);
}

TEST_CASE("control layers: random operations against a brute-force arbitration, and every reported change is exactly a change of the winner") {
  seq::ControlTable ct;
  seq::ControlTable::PerfStore ct_perf;
  ct.attach_perf(ct_perf);
  ct.reset();
  Model m;
  Rng rng(17);
  for (int step = 0; step < 60000; ++step) {
    const std::uint8_t t = static_cast<std::uint8_t>(rng.below(3));
    seq::ModState before[32];
    for (std::uint32_t g = 0; g < seq::kFxGroupCount; ++g) {
      before[g] = m.resolve(t, static_cast<FxGroup>(g));
    }
    seq::ModChange ch[2];
    std::uint32_t n = 0;
    const std::uint32_t op = rng.below(10);
    if (op < 5) {
      const std::uint8_t col = static_cast<std::uint8_t>(rng.below(2));
      const std::uint8_t type = pick_type(rng);
      const std::uint8_t val = static_cast<std::uint8_t>(rng.below(256));
      const seq::FxKind k = seq::fx_kind(type);
      const bool holds = type != 0 && (k == seq::FxKind::Voice || k == seq::FxKind::RowVoice);
      m.type[t][col] = holds ? type : std::uint8_t{0};
      m.value[t][col] = holds ? val : std::uint8_t{0};
      n = ct.set_column(t, col, type, val, ch);
    } else if (op < 7) {
      const std::uint8_t col = static_cast<std::uint8_t>(rng.below(2));
      m.type[t][col] = 0;
      m.value[t][col] = 0;
      n = ct.end_column(t, col, ch);
    } else if (op < 9) {
      const FxGroup g = static_cast<FxGroup>(1 + rng.below(seq::kFxGroupCount - 1));
      const std::int16_t v = static_cast<std::int16_t>(static_cast<std::int32_t>(rng.below(2000)) - 1000);
      m.perf_on[t][static_cast<std::uint32_t>(g)] = true;
      m.perf_val[t][static_cast<std::uint32_t>(g)] = v;
      n = ct.set_perf(t, g, v, ch);
    } else {
      const FxGroup g = static_cast<FxGroup>(1 + rng.below(seq::kFxGroupCount - 1));
      m.perf_on[t][static_cast<std::uint32_t>(g)] = false;
      m.perf_val[t][static_cast<std::uint32_t>(g)] = 0;
      n = ct.end_perf(t, g, ch);
    }
    // the table's winner of every group is the model's; the changes reported are exactly the groups whose winner changed
    std::uint32_t changed = 0;
    for (std::uint32_t g = 1; g < seq::kFxGroupCount; ++g) {
      const seq::ModState now = m.resolve(t, static_cast<FxGroup>(g));
      CHECK(ct.resolve(t, static_cast<FxGroup>(g)) == now);
      if (now != before[g]) {
        ++changed;
        bool reported = false;
        for (std::uint32_t i = 0; i < n; ++i) {
          if (ch[i].group == static_cast<FxGroup>(g) && ch[i].now == now && ch[i].track == t) {
            reported = true;
          }
        }
        CHECK(reported);
      }
    }
    CHECK(changed == n);
    if (step % 5000 == 4999) {
      ct.reset();
      m = Model{};
    }
  }
}

TEST_CASE("control layers: both columns at once give the net changes, with no transient state") {
  seq::ControlTable ct;
  seq::ControlTable::PerfStore ct_perf;
  ct.attach_perf(ct_perf);
  ct.reset();
  seq::ModChange ch[4];
  const std::uint8_t t0[2] = {seq::fx::kLowPass, seq::fx::kHighPass};
  const std::uint8_t v0[2] = {30, 60};
  // two filters in two columns: one parameter, one winner
  CHECK(ct.set_columns(0, 3, t0, v0, ch) == 1u);
  CHECK(ch[0].group == FxGroup::Filter);
  CHECK(ch[0].now.fx_type == seq::fx::kLowPass);
  // the first column becomes a roll and the second an LFO: the filter group ends, the LFO group starts, and no event says "the high-pass won for a moment"
  const std::uint8_t t1[2] = {seq::fx::kRoll, seq::fx::kVolumeLfo};
  const std::uint8_t v1[2] = {3, 5};
  const std::uint32_t n = ct.set_columns(0, 3, t1, v1, ch);
  CHECK(n == 2u);
  bool end = false, set = false;
  for (std::uint32_t i = 0; i < n; ++i) {
    end = end || (ch[i].group == FxGroup::Filter && !ch[i].now.active);
    set = set || (ch[i].group == FxGroup::LfoVolume && ch[i].now.active);
  }
  CHECK(end);
  CHECK(set);
  // a mask changes only its columns
  const std::uint8_t t2[2] = {seq::fx::kPanning, seq::fx::kPanning};
  const std::uint8_t v2[2] = {11, 22};
  CHECK(ct.set_columns(0, 1, t2, v2, ch) == 1u);  // column 0 only
  CHECK(ct.column_type(0, 1) == seq::fx::kVolumeLfo);
  CHECK(ct.column_value(0, 0) == 11);
  // identical state again: nothing
  CHECK(ct.set_columns(0, 1, t2, v2, ch) == 0u);
}

TEST_CASE("control: reset_columns ends the two sequencer columns of every track and leaves the performance layer's values") {
  seq::ControlTable ct;
  seq::ControlTable::PerfStore ct_perf;
  ct.attach_perf(ct_perf);
  ct.reset();
  seq::ModChange out[4];
  (void)ct.set_column(0, 0, 27, 40, out);
  (void)ct.set_column(0, 1, 31, 10, out);
  (void)ct.set_column(5, 0, 30, 70, out);
  (void)ct.set_perf(0, FxGroup::Filter, 0x0105, out);
  (void)ct.set_perf(5, FxGroup::Panning, -20, out);
  ct.reset_columns();
  CHECK(ct.column_type(0, 0) == 0);
  CHECK(ct.column_type(0, 1) == 0);
  CHECK(ct.column_type(5, 0) == 0);
  CHECK(ct.perf_holds(0, FxGroup::Filter));
  CHECK(ct.perf_holds(5, FxGroup::Panning));
  const seq::ModState s = ct.resolve(0, FxGroup::Filter);
  CHECK((s.active && s.layer == audio::kLayerPerf && s.perf == 0x0105 && s.fx_type == 0));  // the layer alone: no FX underneath any more
  ct.reset();
  CHECK_FALSE(ct.perf_holds(0, FxGroup::Filter));
}

// The performance values are the caller's store (SRAM_U recovery: SDRAM on the target): a table without one has no performance layer, and a store attached later starts empty.
TEST_CASE("control layers: the performance store is attached, not owned") {
  seq::ControlTable ct;  // no store
  seq::ModChange ch[2];
  CHECK(ct.set_perf(1, FxGroup::Filter, 77, ch) == 0u);
  CHECK_FALSE(ct.perf_holds(1, FxGroup::Filter));
  CHECK_FALSE(ct.resolve(1, FxGroup::Filter).active);
  CHECK(ct.end_perf(1, FxGroup::Filter, ch) == 0u);
  ct.clear_track(1);
  ct.reset();
  seq::ControlTable::PerfStore store;
  for (auto& row : store.val) {
    for (auto& v : row) {
      v = 0x5a5a;
    }
  }
  ct.attach_perf(store);  // zeroed, and no group held
  CHECK_FALSE(ct.perf_holds(1, FxGroup::Filter));
  CHECK(ct.set_perf(1, FxGroup::Filter, 77, ch) == 1u);
  CHECK(ct.resolve(1, FxGroup::Filter).perf == 77);
  CHECK(store.val[1][static_cast<std::uint32_t>(FxGroup::Filter)] == 77);
  ct.clear_track(1);
  CHECK(store.val[1][static_cast<std::uint32_t>(FxGroup::Filter)] == 0);
}
