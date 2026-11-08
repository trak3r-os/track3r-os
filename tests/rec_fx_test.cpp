// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-FXREC S1 (docs/specs/OS-FXREC.md): the map of an instrument parameter's new value to its step FX (app/rec_fx.h rec_fx_map, section 3.1)
// and the column rule of a recorded FX (rec_fx_place, section 3.3), both pure. Host and Cortex-M4F.
#include <doctest/doctest.h>

#include <o3os/app/rec_fx.h>

#include <cstdint>
#include <initializer_list>

namespace model = o3os::model;
namespace seq = o3os::seq;
namespace rec = o3os::app::rec;
namespace ip = o3os::model::instrument_param;

namespace {

TEST_CASE("fxrec map: CUTOFF is 27/28/29 by the filter type, the value round(cutoff * 100)") {
  model::StepFx fx{9, 9};
  for (unsigned ft = 0; ft < 3; ++ft) {
    const std::uint8_t type = static_cast<std::uint8_t>(seq::fx::kLowPass + ft);  // Low-pass / High-pass / Band-pass
    CHECK(rec::rec_fx_map(ip::Cutoff, ft, 0.0f, fx));
    CHECK(fx.type == type);
    CHECK(fx.value == 0);
    CHECK(rec::rec_fx_map(ip::Cutoff, ft, 0.5f, fx));
    CHECK(fx.type == type);
    CHECK(fx.value == 50);
    CHECK(rec::rec_fx_map(ip::Cutoff, ft, 1.0f, fx));
    CHECK(fx.type == type);
    CHECK(fx.value == 100);
  }
  // held to 0..100 whatever the parameter carries
  CHECK(rec::rec_fx_map(ip::Cutoff, 0, 1.5f, fx));
  CHECK(fx.value == 100);
  CHECK(rec::rec_fx_map(ip::Cutoff, 0, -0.2f, fx));
  CHECK(fx.value == 0);
  // no such filter type: nothing, and out untouched
  fx = {9, 9};
  CHECK_FALSE(rec::rec_fx_map(ip::Cutoff, 3, 0.5f, fx));
  CHECK(fx.type == 9);
  CHECK(fx.value == 9);
}

TEST_CASE("fxrec map: PANNING, DELAY SEND, REVERB SEND, OVERDRIVE at both ends of their range and the middle; BIT DEPTH 4..16") {
  model::StepFx fx{};
  struct Row {
    unsigned field;
    std::uint8_t type;
  };
  constexpr Row kRows[] = {
      {ip::Panning, seq::fx::kPanning},
      {ip::DelaySend, seq::fx::kDelaySend},
      {ip::ReverbSend, seq::fx::kReverbSend},
      {ip::Overdrive, seq::fx::kOverdrive},
  };
  for (const Row& r : kRows) {
    for (float v : {0.0f, 50.0f, 100.0f}) {
      CHECK(rec::rec_fx_map(r.field, 0, v, fx));
      CHECK(fx.type == r.type);
      CHECK(fx.value == static_cast<std::uint8_t>(v));
    }
    CHECK(rec::rec_fx_map(r.field, 0, 120.0f, fx));  // held to the top
    CHECK(fx.value == 100);
    CHECK(rec::rec_fx_map(r.field, 0, -5.0f, fx));  // and to the bottom
    CHECK(fx.value == 0);
  }
  for (float v : {4.0f, 10.0f, 16.0f}) {  // the parameter is 4..16 (the FX takes 1..16)
    CHECK(rec::rec_fx_map(ip::BitDepth, 0, v, fx));
    CHECK(fx.type == seq::fx::kBitDepth);
    CHECK(fx.value == static_cast<std::uint8_t>(v));
  }
  CHECK(rec::rec_fx_map(ip::BitDepth, 0, 2.0f, fx));
  CHECK(fx.value == 4);
  CHECK(rec::rec_fx_map(ip::BitDepth, 0, 20.0f, fx));
  CHECK(fx.value == 16);
}

TEST_CASE("fxrec map: TUNE, RESONANCE, VOLUME and every other parameter map to nothing") {
  model::StepFx fx{9, 9};
  for (unsigned field :
       {ip::Volume, ip::Tune, ip::Resonance, ip::FilterEnable, ip::FilterType, ip::FineTune, ip::Glide, ip::kEnvelopeFirst, 0x100u}) {
    CHECK_FALSE(rec::rec_fx_map(field, 0, 0.5f, fx));
    CHECK(fx.type == 9);
    CHECK(fx.value == 9);    // out untouched
  }
}

TEST_CASE("fxrec place: the column rule of 3.3") {
  model::StepFx fx{};
  // an empty step takes column 1
  model::Step s = model::kEmptyStep;
  CHECK(rec::rec_fx_map(ip::Panning, 0, 30.0f, fx));
  CHECK(rec::rec_fx_place(s, fx.type, fx.value) == 0);
  CHECK(s.fx[0].type == seq::fx::kPanning);
  CHECK(s.fx[0].value == 30);
  CHECK(s.fx[1].type == model::kFxNone);
  CHECK(s.fx[1].value == 0);
  // a later message on the same step overwrites the value in the same column: one value per step
  CHECK(rec::rec_fx_map(ip::Panning, 0, 60.0f, fx));
  CHECK(rec::rec_fx_place(s, fx.type, fx.value) == 0);
  CHECK(s.fx[0].type == seq::fx::kPanning);
  CHECK(s.fx[0].value == 60);
  CHECK(s.fx[1].type == model::kFxNone);

  // column 1 holds Low-pass 40: column 1 again, the type follows the current filter type
  model::Step f = model::kEmptyStep;
  f.fx[0] = {seq::fx::kLowPass, 40};
  CHECK(rec::rec_fx_map(ip::Cutoff, 1, 0.7f, fx));  // High-pass 70
  CHECK(rec::rec_fx_place(f, fx.type, fx.value) == 0);
  CHECK(f.fx[0].type == seq::fx::kHighPass);
  CHECK(f.fx[0].value == 70);
  CHECK(f.fx[1].type == model::kFxNone);

  // the filter group is matched by group: Band-pass in column 1, a Low-pass records into column 1
  model::Step g = model::kEmptyStep;
  g.fx[0] = {seq::fx::kBandPass, 12};
  CHECK(rec::rec_fx_map(ip::Cutoff, 0, 0.33f, fx));  // Low-pass 33
  CHECK(rec::rec_fx_place(g, fx.type, fx.value) == 0);
  CHECK(g.fx[0].type == seq::fx::kLowPass);
  CHECK(g.fx[0].value == 33);

  // column 1 holds Chance, column 2 empty: column 2, the user's FX stays
  model::Step c = model::kEmptyStep;
  c.fx[0] = {seq::fx::kChance, 80};
  CHECK(rec::rec_fx_place(c, seq::fx::kPanning, 10) == 1);
  CHECK(c.fx[0].type == seq::fx::kChance);
  CHECK(c.fx[0].value == 80);
  CHECK(c.fx[1].type == seq::fx::kPanning);
  CHECK(c.fx[1].value == 10);

  // column 1 Chance, column 2 Micro-move: nothing (a Micro-move is an FX like any other)
  model::Step m = model::kEmptyStep;
  m.fx[0] = {seq::fx::kChance, 80};
  m.fx[1] = {seq::fx::kMicroMove, 20};
  CHECK(rec::rec_fx_place(m, seq::fx::kPanning, 10) == -1);
  CHECK(m.fx[0].type == seq::fx::kChance);
  CHECK(m.fx[0].value == 80);
  CHECK(m.fx[1].type == seq::fx::kMicroMove);
  CHECK(m.fx[1].value == 20);

  // column 2 holds Delay send, column 1 empty: column 2 (the column of the group wins over the free column 1)
  model::Step d = model::kEmptyStep;
  d.fx[1] = {seq::fx::kDelaySend, 55};
  CHECK(rec::rec_fx_map(ip::DelaySend, 0, 90.0f, fx));
  CHECK(rec::rec_fx_place(d, fx.type, fx.value) == 1);
  CHECK(d.fx[0].type == model::kFxNone);
  CHECK(d.fx[1].type == seq::fx::kDelaySend);
  CHECK(d.fx[1].value == 90);

  // column 1 holds Chance, column 2 the same group (Panning): column 2, the value overwritten (the column that holds the group wins, whichever column it is)
  model::Step p = model::kEmptyStep;
  p.fx[0] = {seq::fx::kChance, 80};
  p.fx[1] = {seq::fx::kPanning, 10};
  CHECK(rec::rec_fx_place(p, seq::fx::kPanning, 66) == 1);
  CHECK(p.fx[0].type == seq::fx::kChance);
  CHECK(p.fx[0].value == 80);
  CHECK(p.fx[1].type == seq::fx::kPanning);
  CHECK(p.fx[1].value == 66);

  // the note, instrument and volume are never touched
  model::Step n = model::kEmptyStep;
  n.note = 60;
  n.instr = 3;
  n.vol = 100;
  n.fx[0] = {seq::fx::kChance, 1};
  n.fx[1] = {seq::fx::kMicroMove, 2};
  CHECK(rec::rec_fx_place(n, seq::fx::kReverbSend, 44) == -1);
  CHECK(n.note == 60);
  CHECK(n.instr == 3);
  CHECK(n.vol == 100);
  n.fx[1] = {model::kFxNone, 0};
  CHECK(rec::rec_fx_place(n, seq::fx::kReverbSend, 44) == 1);
  CHECK(n.note == 60);
  CHECK(n.instr == 3);
  CHECK(n.vol == 100);
  CHECK(n.fx[1].type == seq::fx::kReverbSend);
  CHECK(n.fx[1].value == 44);
}

TEST_CASE("fxrec: every mapped FX type's group is the filter's or its own (fx_group)") {
  model::StepFx fx{};
  struct Row {
    unsigned field;
    seq::FxGroup group;
  };
  constexpr Row kRows[] = {
      {ip::Cutoff, seq::FxGroup::Filter},
      {ip::Panning, seq::FxGroup::Panning},
      {ip::DelaySend, seq::FxGroup::DelaySend},
      {ip::ReverbSend, seq::FxGroup::ReverbSend},
      {ip::Overdrive, seq::FxGroup::Overdrive},
      {ip::BitDepth, seq::FxGroup::BitDepth},
  };
  for (const Row& r : kRows) {
    CHECK(rec::rec_fx_map(r.field, 0, 0.5f, fx));
    CHECK(seq::fx_group(fx.type, 0) == r.group);
    CHECK(seq::fx_group(fx.type, 1) == r.group);  // a table type: the group does not depend on the column
  }
  // the three filters are one group
  for (unsigned ft = 0; ft < 3; ++ft) {
    CHECK(rec::rec_fx_map(ip::Cutoff, ft, 0.5f, fx));
    CHECK(seq::fx_group(fx.type, 0) == seq::FxGroup::Filter);
  }
  // the non-filter mapped types are each a group of its own: pairwise distinct, and none is the filter's
  seq::FxGroup seen[5];
  unsigned count = 0;
  for (const Row& r : kRows) {
    if (r.group == seq::FxGroup::Filter) {
      continue;
    }
    CHECK(rec::rec_fx_map(r.field, 0, 0.5f, fx));
    for (unsigned i = 0; i < count; ++i) {
      CHECK(seen[i] != seq::fx_group(fx.type, 0));
    }
    seen[count++] = seq::fx_group(fx.type, 0);
  }
  CHECK(count == 5);
}

}  // namespace
