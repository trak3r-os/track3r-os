// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSTUI S3: the rows of the source page (Params F1) in the parameter tables: where they are in the id space, that their defaults are what init_instrument makes, that their ranges are those of
// docs/specs/OS-INSTUI.md (the citations are at the checks), that each one reads and writes the SamplerParams member it names, and that the groups of the tables still hold every parameter once.
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3os/audio/voices.h>
#include <o3os/core/param.h>
#include <o3os/model/instrument.h>
#include <o3os/model/params.h>
#include <o3os/model/project.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

using namespace o3os;
namespace ip = model::instrument_param;

namespace {

core::ParamId source_id(unsigned slot, unsigned field) { return core::param_id(core::ParamKind::Instrument, slot, field); }

const core::ParamDesc& desc_of(unsigned field) {
  static const core::ParamDesc none{};
  const core::ParamDesc* d = model::param_desc(source_id(0, field));
  CHECK(d != nullptr);
  return d != nullptr ? *d : none;
}

// The static facts: the ids sit past the first table and below the envelopes' and clear of the audio side's live edits (voices.h inst_param LoopStart .. GranPos = 0x10..0x13), and the ids that existed keep their numbers.
static_assert(ip::kSourceFirst > audio::inst_param::GranPos && ip::kSourceFirst > audio::inst_param::WtWindow && ip::kSourceFirst > audio::inst_param::LoopEnd, "no collision with the engine's live ids");
static_assert(ip::kSourceFirst + ip::kSourceRows <= ip::kEnvelopeFirst, "the rows fit below the envelopes");
static_assert(ip::Volume == 0 && ip::BitDepth == 12 && ip::PlayEngine == 13 && ip::kBase == 14 && ip::kEnvelopeFirst == 0x20, "the ids that existed keep their numbers");
static_assert(ip::PlayMode == 0x14 && ip::Slice == 0x1C && ip::kSourceRows == 9 && ip::kCount == 14 + 9 + 36 + 24 + 12 && ip::kGroups == 5 && ip::GroupSource == 4 && ip::GroupSends == 3, "the appended ids");

}  // namespace

TEST_CASE("source params: every row exists, is a number or a choice of its own, none is modulatable, and the ones made by an action are not learnable") {
  for (unsigned f = ip::kSourceFirst; f < ip::kSourceFirst + ip::kSourceRows; ++f) {
    const core::ParamDesc& d = desc_of(f);
    CHECK_MESSAGE(core::param_desc_valid(d), d.name);
    // learnable: the grain's rows; not learnable: the ones made by an action (the play mode, the window size, the slice count) and the two a CC cannot scale onto yet (WT POS clamps to the windows the
    // sample has and SLICE to the slices there are, so a CC over 0..65535 / 0..47 would land on the last one for every value above 0)
    const bool by_action = f == ip::PlayMode || f == ip::WtSize || f == ip::Slices || f == ip::WtPos || f == ip::Slice;
    CHECK_MESSAGE(((d.flags & core::kParamLearnable) != 0) == !by_action, d.name);
    CHECK_MESSAGE((d.flags & (core::kParamModulatable | core::kParamApply)) == 0, d.name);
    CHECK_MESSAGE(d.group == ip::GroupSource, d.name);
    CHECK_MESSAGE(std::strlen(d.name) <= core::kParamNameChars, d.name);
  }
  CHECK(model::param_desc(source_id(0, ip::kSourceFirst + ip::kSourceRows)) == nullptr);  // 0x1D (PlayThru, OS-CEPLAYTHRU P2) is a virtual field like 0x1E/0x1F: no table row
  CHECK(model::param_desc(source_id(0, 0x13)) == nullptr);                                  // and the engine's live ids are not parameters
  CHECK(model::param_desc(source_id(63, ip::PlayMode)) != nullptr);
  CHECK(model::param_desc(source_id(64, ip::PlayMode)) == nullptr);
}

TEST_CASE("source params: the defaults are what init_instrument makes, row by row") {
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Sample);
  const unsigned fields[] = {ip::PlayMode, ip::WtPos, ip::WtSize, ip::GrainPos, ip::GrainLength, ip::GrainShape, ip::GrainLoop, ip::Slices, ip::Slice, ip::PlayEngine};
  for (const unsigned f : fields) {
    model::ParamField pf;
    NEED(model::find_param(source_id(0, f), pf));
    CHECK_MESSAGE(model::param_load(&in, pf) == pf.desc->def, pf.desc->name);
  }
  CHECK(in.sampler.grain_length == 0);  // (not set: init_instrument leaves the bytes of a new instrument as they were; the engine plays the default grain length, 441)
}

TEST_CASE("source params: the ranges and labels follow the instrument file") {
  // play mode: 8 modes, in the file's order
  const core::ParamDesc& pm = desc_of(ip::PlayMode);
  CHECK(pm.min == 0.0f);
  CHECK(pm.max == 7.0f);
  CHECK(pm.list_count == 8);
  const char* const modes[8] = {"One shot", "Loop fwd", "Loop bwd", "Ping-pong", "Slice", "Beat slice", "Wavetable", "Granular"};
  for (unsigned i = 0; i < 8; ++i) {
    CHECK(std::string(pm.names[i]) == modes[i]);
  }
  // wavetable window size: counters 5..11 = 32 .. 2048; 0 is "not set"
  CHECK(desc_of(ip::WtSize).min == 0.0f);
  CHECK(desc_of(ip::WtSize).max == 2048.0f);
  // grain: length 44..44100, the position up to 65535, 3 shapes, 3 loop types
  CHECK(desc_of(ip::GrainLength).min == 0.0f);  // 0: not set (441); the floor of 44 is the edit's
  CHECK(desc_of(ip::GrainLength).max == 44100.0f);
  CHECK(desc_of(ip::GrainPos).max == 65535.0f);
  const core::ParamDesc& shape = desc_of(ip::GrainShape);
  CHECK(shape.list_count == 3);
  CHECK(std::string(shape.names[0]) == "Square");
  CHECK(std::string(shape.names[1]) == "Triangle");
  CHECK(std::string(shape.names[2]) == "Gauss");
  const core::ParamDesc& loop = desc_of(ip::GrainLoop);
  CHECK(loop.list_count == 3);
  CHECK(std::string(loop.names[0]) == "Forward");
  CHECK(std::string(loop.names[1]) == "Backward");
  CHECK(std::string(loop.names[2]) == "PingPong");
  // slices: at most 48, the selected slice 0..47
  CHECK(desc_of(ip::Slices).max == 48.0f);
  CHECK(desc_of(ip::Slice).max == 47.0f);
}

TEST_CASE("source params: each row reads and writes the member it names, and a value out of range is refused") {
  auto arena = std::make_unique<model::ModelArena>();
  model::Project& p = arena->project;
  p.reset();
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Sample);
  NEED(p.set_instrument(3, in) == model::Status::Ok);
  struct Case {
    unsigned field;
    float value;
  };
  const Case cases[] = {{ip::PlayMode, 6}, {ip::WtPos, 300}, {ip::WtSize, 512}, {ip::GrainPos, 40000}, {ip::GrainLength, 2205}, {ip::GrainShape, 2}, {ip::GrainLoop, 1}, {ip::Slices, 16}, {ip::Slice, 7}};
  for (const Case& c : cases) {
    CHECK_MESSAGE(p.set_param(source_id(3, c.field), c.value) == model::Status::Ok, c.field);
    CHECK_MESSAGE(p.get_param(source_id(3, c.field)) == c.value, c.field);
  }
  const model::SamplerParams& s = p.instrument(3)->sampler;
  CHECK(s.play_mode == 6);
  CHECK(s.wavetable_window == 300u);
  CHECK(s.wavetable_window_size == 512);
  CHECK(s.grain_position == 40000);
  CHECK(s.grain_length == 2205);
  CHECK(s.grain_shape == 2);
  CHECK(s.grain_type == 1);
  CHECK(s.slice_count == 16);
  CHECK(s.selected_slice == 7);
  CHECK(p.set_param(source_id(3, ip::PlayMode), 8.0f) != model::Status::Ok);
  CHECK(p.set_param(source_id(3, ip::GrainShape), 3.0f) != model::Status::Ok);
  CHECK(p.set_param(source_id(3, ip::Slice), 48.0f) != model::Status::Ok);
  CHECK(p.get_param(source_id(3, ip::PlayMode)) == 6.0f);  // a refused write leaves the value
}

TEST_CASE("source params: the group of the source page holds the nine rows and PLAY ENGINE; VOICE no longer has the engine; every parameter is in exactly one group") {
  std::uint16_t f[16];
  const unsigned n = model::param_group_fields(core::ParamKind::Instrument, ip::GroupSource, 0, f, 16);
  CHECK(n == ip::kSourceRows + 1u);
  for (unsigned i = 0; i < ip::kSourceRows; ++i) {
    CHECK(f[i] == ip::kSourceFirst + i);
  }
  CHECK(f[ip::kSourceRows] == ip::PlayEngine);
  CHECK(std::string(model::param_group_name(core::ParamKind::Instrument, ip::GroupSource)) == "SOURCE");
  const unsigned voice = model::param_group_fields(core::ParamKind::Instrument, ip::GroupVoice, 0, f, 16);
  CHECK(voice == 6u);  // (VEL> closes it, OS-INSTUI S9)
  for (unsigned i = 0; i < voice; ++i) {
    CHECK(f[i] != ip::PlayEngine);
  }
  // the dense slot of every id and back: the envelopes come after the nine rows
  for (unsigned slot = 0; slot < ip::kCount; ++slot) {
    const unsigned field = model::param_field_of_slot(core::ParamKind::Instrument, slot);
    CHECK(model::param_slot(source_id(0, field)) == slot);
  }
  CHECK(model::param_slot(source_id(0, ip::PlayMode)) == ip::kBase);
  CHECK(model::param_slot(source_id(0, ip::envelope(0, 0))) == ip::kBase + ip::kSourceRows);
}
