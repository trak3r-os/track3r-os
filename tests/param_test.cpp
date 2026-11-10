// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-002d, the pure parts of parameters: the id, the descriptor maths (curves, steps, display text and parsing it back), the tables of
// the model (every row valid, dense, representable, defaults), the settings block, the MIDI Learn rules and the ParamChange ring under
// load. No project arena: this file also runs on the Cortex-M4F (qemu_params).
#include <doctest/doctest.h>

#include <o3os/core/learn.h>
#include <o3os/core/param.h>
#include <o3os/model/instrument.h>
#include <o3os/model/params.h>
#include <o3os/model/project.h>
#include <o3os/model/settings.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "support/need.h"

using namespace o3os;
using core::ParamCurve;
using core::ParamDesc;
using core::ParamId;
using core::ParamKind;
using core::ParamKnot;
using core::ParamUnit;

#if defined(__arm__)
constexpr unsigned kScale = 4;  // the target is slower: fewer blocks, the same cases
#else
constexpr unsigned kScale = 1;
#endif

namespace {

constexpr ParamKind kKinds[] = {ParamKind::Instrument, ParamKind::Track, ParamKind::Master, ParamKind::Project, ParamKind::Device};

// Every parameter that exists: the first and the last index of each kind (the tables are the same for every index), every slot.
template <typename F>
void for_each_param(F&& f) {
  for (const ParamKind kind : kKinds) {
    const unsigned count = model::param_index_count(kind);
    const unsigned indices[2] = {0, count - 1u};
    for (unsigned n = 0; n < (count > 1 ? 2u : 1u); ++n) {
      const unsigned index = indices[n];
      for (unsigned slot = 0; slot < model::param_slots_per_index(kind); ++slot) {
        f(core::param_id(kind, index, model::param_field_of_slot(kind, slot)), slot);
      }
    }
  }
}

std::string text(const ParamDesc& d, float v) {
  char buf[core::kParamTextChars + 1];
  const unsigned n = core::param_format(d, v, buf, sizeof buf);
  CHECK(n == std::strlen(buf));
  return std::string(buf, n);
}

constexpr const char* const kThree[] = {"Low", "Medium", "High"};
constexpr ParamKnot kTimeKnots[] = {{0.0f, 0.0f}, {100.0f / 190.0f, 1000.0f}, {1.0f, 10000.0f}};

// Synthetic descriptors, one per unit and curve (no table row has a log or an exp curve yet).
constexpr ParamDesc kInt = {"INT", "st", nullptr, nullptr, -24.0f, 24.0f, 0.0f, 48, 12, 0, ParamUnit::Int, ParamCurve::Linear, 0, 0};
constexpr ParamDesc kPan = {"PAN", "", nullptr, nullptr, 0.0f, 100.0f, 50.0f, 100, 10, 0, ParamUnit::Pan, ParamCurve::Linear, 0, 0};
constexpr ParamDesc kNorm = {"NORM", "%", nullptr, nullptr, 0.0f, 4.3f, 0.0f, 100, 10, 0, ParamUnit::NormPct, ParamCurve::Linear, 0, 0};
constexpr ParamDesc kSec = {"SEC", "s", nullptr, kTimeKnots, 0.0f, 10000.0f, 0.0f, 190, 10, 3, ParamUnit::Seconds, ParamCurve::Table, 0, 0};
constexpr ParamDesc kLog = {"LOG", "Hz", nullptr, nullptr, 20.0f, 20000.0f, 1000.0f, 300, 10, 0, ParamUnit::Int, ParamCurve::Log, 0, 0};
constexpr ParamDesc kEnum = {"ENUM", "", kThree, nullptr, 0.0f, 2.0f, 2.0f, 2, 1, 3, ParamUnit::Enum, ParamCurve::Linear, 0, 0};
constexpr ParamDesc kExp = {"EXP", "", nullptr, nullptr, 0.0f, 100.0f, 0.0f, 100, 10, 0, ParamUnit::Int, ParamCurve::Exp, 0, 0};

}  // namespace

// ---- ids and the ring's message ------------------------------------------------------------------------------------------

TEST_CASE("param: an id packs the kind, the index and the field, and a ParamChange is three words") {
  const ParamId id = core::param_id(ParamKind::Instrument, 63, 0x0025);
  CHECK(static_cast<std::uint32_t>(id) == 0x013F0025u);
  CHECK(core::param_kind(id) == ParamKind::Instrument);
  CHECK(core::param_index(id) == 63u);
  CHECK(core::param_field(id) == 0x25u);
  CHECK(core::param_id(ParamKind::None, 0, 0) == ParamId::None);
  CHECK(core::param_id(ParamKind::Device, 0x1FF, 0x1FFFF) == core::param_id(ParamKind::Device, 0xFF, 0xFFFF));  // out-of-range parts are cut
  CHECK(sizeof(core::ParamChange) == 12u);
  CHECK(core::kParamRingSize == 256u);
  CHECK(core::kMaxParamsPerBlock == 64u);
  // The ids that files store never change: pin a few (a Learn binding of a project holds them).
  CHECK(static_cast<std::uint32_t>(core::param_id(ParamKind::Instrument, 0, model::instrument_param::Cutoff)) == 0x01000007u);
  CHECK(static_cast<std::uint32_t>(core::param_id(ParamKind::Instrument, 5, model::instrument_param::envelope(2, 2))) == 0x01050032u);
  CHECK(static_cast<std::uint32_t>(core::param_id(ParamKind::Project, 0, model::project_setting::SendsMode)) == 0x04000002u);
  CHECK(static_cast<std::uint32_t>(core::param_id(ParamKind::Device, 0, model::device_setting::ClockSync)) == 0x05000006u);
}

// ---- the tables ---------------------------------------------------------------------------------------------------------

TEST_CASE("param tables: every row is a valid descriptor that its storage can hold, and the slots are dense and unique") {
  unsigned rows = 0;
  std::set<std::uint32_t> ids;
  for_each_param([&](ParamId id, unsigned slot) {
    ++rows;
    CHECK(ids.insert(static_cast<std::uint32_t>(id)).second);
    model::ParamField f;
    NEED(model::find_param(id, f));
    const ParamDesc* d = model::param_desc(id);
    NEED(d != nullptr);
    CHECK(f.desc == d);  // one row: the field's descriptor is the one param_desc returns
    CHECK_MESSAGE(core::param_desc_valid(*d), d->name);
    CHECK(model::param_slot(id) == slot);
    // every parameter is a Learn target (product-ux decision 4), but the PLAY ENGINE choice (OS-FU5): it is made by the engine action, which a CC does not drive; and USB RETURN
    // (OS-USBRET) and USB AUDIO (OS-USBAUDIO): a controller must not be able to re-enumerate the USB port by accident. Those two are the rows that are press to apply (kParamApply), and no others are.
    const bool reenumerates = std::strcmp(d->name, "USB RETURN") == 0 || std::strcmp(d->name, "USB AUDIO") == 0;
    // OS-MIDICCMAP S1: and CC MAP, a device MIDI routing switch, not learnable.
    // OS-MTC A3: and MTC OUT and MTC START (a controller must not start a time code).
    const bool routing = std::strcmp(d->name, "CC MAP") == 0 || std::strcmp(d->name, "MTC OUT") == 0 || std::strcmp(d->name, "MTC START") == 0 || std::strcmp(d->name, "MTC IN") == 0 || std::strcmp(d->name, "STATUS LED") == 0;  // OS-LEDBPM: STATUS LED is a display mode of the LED, not worth a CC;  // OS-MTC B3: and MTC IN (a controller must not hand the transport to a master)
    // OS-INSTUI S3: and PLAY MODE, WT SIZE and SLICES, which are made by their actions (a window size moves by counters, a mode needs the sample's say, the slices are made on Wave)
    const bool by_action = std::strcmp(d->name, "PLAY ENGINE") == 0 || std::strcmp(d->name, "PLAY MODE") == 0 || std::strcmp(d->name, "WT SIZE") == 0 || std::strcmp(d->name, "SLICES") == 0 ||
                           std::strcmp(d->name, "WT POS") == 0 || std::strcmp(d->name, "SLICE") == 0;  // (WT POS and SLICE: not until a CC scale onto the windows / slices there are exists)
    CHECK(((d->flags & core::kParamLearnable) != 0) == (!by_action && !reenumerates && !routing));
    CHECK(((d->flags & core::kParamApply) != 0) == reenumerates);
    CHECK_MESSAGE(((d->flags & core::kParamApply) == 0 || (d->flags & core::kParamLearnable) == 0), d->name);
    // The storage can hold the whole range, and a default is a value of it.
    switch (f.store) {
      case model::ParamStore::U8: CHECK((d->min >= 0.0f && d->max <= 255.0f)); break;
      case model::ParamStore::I8: CHECK((d->min >= -128.0f && d->max <= 127.0f)); break;
      case model::ParamStore::U16: CHECK((d->min >= 0.0f && d->max <= 65535.0f)); break;
      case model::ParamStore::F32: break;
      case model::ParamStore::EnvState: CHECK((d->min == 0.0f && d->max == 4.0f)); break;  // OS-INSTUI S6, OS-CEWTENV W7a: SOURCE, Off / Envelope / LFO / WT Env / WT LFO
    }
    if (f.mask != 0) {
      CHECK((d->min == 0.0f && d->max == 1.0f && (f.mask & (f.mask - 1u)) == 0));  // a switch: one bit
    }
    CHECK(core::param_in_range(*d, d->def));
    // A modulation target is something an envelope or an LFO can move (volume, pan, cutoff and fine tune).
    if ((d->flags & core::kParamModulatable) != 0) {
      CHECK((std::strcmp(d->name, "VOLUME") == 0 || std::strcmp(d->name, "PANNING") == 0 || std::strcmp(d->name, "CUTOFF") == 0 ||
             std::strcmp(d->name, "FINE TUNE") == 0));
    }
  });
  // 64 + 16 instruments' and tracks' worth, and one each of the others: 2 x 95 + 2 x 5 + 8 + 3 + 30 rows were visited (29 device rows before OS-MTC B3 appended MTC IN, 27 device rows before OS-MTC A3 appended MTC OUT and MTC START, 26 before OS-MIDICCMAP S1 appended CC MAP).
  CHECK(rows == 2u * 95u + 2u * 5u + 8u + 3u + 31u);  // 31 device rows since OS-LEDBPM appended STATUS LED
  CHECK(model::instrument_param::kCount == 95u);  // 14 + the 9 rows of the source page (OS-INSTUI S3) + 6 x 6 envelope fields + 6 x 4 MOD rows (SOURCE, LFO SHAPE, SPEED, AMOUNT; OS-INSTUI S6) + the 12 MIDI / VEL> rows (OS-INSTUI S8b + S9)
  CHECK(model::kProjectParamSlots == 64u * 95u + 16u * 5u + 8u);
}

TEST_CASE("param tables: what is not a parameter is not found") {
  model::ParamField f;
  CHECK_FALSE(model::find_param(ParamId::None, f));
  CHECK_FALSE(model::find_param(core::param_id(ParamKind::Instrument, 64, 0), f));        // slot 64
  CHECK_FALSE(model::find_param(core::param_id(ParamKind::Instrument, 0, 14), f));        // between the two blocks of fields
  CHECK_FALSE(model::find_param(core::param_id(ParamKind::Instrument, 0, 0x1F), f));
  CHECK_FALSE(model::find_param(core::param_id(ParamKind::Instrument, 0, model::instrument_param::envelope(6, 0)), f));  // envelope 6
  CHECK_FALSE(model::find_param(core::param_id(ParamKind::Instrument, 0, model::instrument_param::envelope(0, 6)), f));  // field 6 of 6
  CHECK_FALSE(model::find_param(core::param_id(ParamKind::Instrument, 0, 0x2F8), f));
  CHECK_FALSE(model::find_param(core::param_id(ParamKind::Instrument, 0, 0x5F), f));                                                // below the MOD rows
  CHECK_FALSE(model::find_param(core::param_id(ParamKind::Instrument, 0, model::instrument_param::mod(6, 0)), f));              // destination 6 of 6 (0x78)
  CHECK(model::find_param(core::param_id(ParamKind::Instrument, 0, model::instrument_param::mod(5, 3)), f));                    // the last one
  CHECK_FALSE(model::find_param(core::param_id(ParamKind::Track, 16, 0), f));
  CHECK_FALSE(model::find_param(core::param_id(ParamKind::Track, 0, 5), f));
  CHECK_FALSE(model::find_param(core::param_id(ParamKind::Master, 1, 0), f));
  CHECK_FALSE(model::find_param(core::param_id(ParamKind::Master, 0, 8), f));
  CHECK_FALSE(model::find_param(core::param_id(ParamKind::Project, 0, 4), f));
  CHECK_FALSE(model::find_param(core::param_id(ParamKind::Project, 1, 0), f));
  CHECK_FALSE(model::find_param(core::param_id(ParamKind::Device, 0, 31), f));  // 31 is past the last field (OS-LEDBPM took 30, OS-MTC B3 29, A3 27 and 28, OS-MIDICCMAP S1 26)
  CHECK(model::param_desc(core::param_id(ParamKind::Device, 0, 31)) == nullptr);
  CHECK(model::param_slot(core::param_id(ParamKind::Device, 0, 31)) == model::kNoParamSlot);
  CHECK(model::param_field_of_slot(ParamKind::Track, 5) == 0xFFFFu);
  CHECK(model::param_index_count(ParamKind::None) == 0u);
  CHECK(model::param_slots_per_index(ParamKind::None) == 0u);
}

TEST_CASE("param tables: a fresh instrument's values are the descriptors' defaults, so reset is what a new instrument is") {
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Sample);
  for (unsigned slot = 0; slot < model::instrument_param::kCount; ++slot) {
    const ParamId id = core::param_id(ParamKind::Instrument, 0, model::param_field_of_slot(ParamKind::Instrument, slot));
    model::ParamField f;
    NEED(model::find_param(id, f));
    CHECK_MESSAGE(model::param_load(&in, f) == f.desc->def, f.desc->name << " slot " << slot);
  }
  // A track and the master of a new project, which are constants of the model.
  model::TrackSlot t{model::TrackType::Audio, 0, static_cast<std::uint8_t>(model::kDefaultVolume), static_cast<std::uint8_t>(model::kDefaultPan), 0, {}};
  for (unsigned f = 0; f < model::track_param::kCount; ++f) {
    model::ParamField pf;
    NEED(model::find_param(core::param_id(ParamKind::Track, 0, f), pf));
    CHECK_MESSAGE(model::param_load(&t, pf) == pf.desc->def, pf.desc->name);
  }
  model::MasterSettings m{86, 86, 86, 86, 0};
  for (unsigned f = 0; f < model::master_param::kCount; ++f) {
    model::ParamField pf;
    NEED(model::find_param(core::param_id(ParamKind::Master, 0, f), pf));
    CHECK_MESSAGE(model::param_load(&m, pf) == pf.desc->def, pf.desc->name);
  }
}

TEST_CASE("param tables: the pages cover every field once, every page has rows, and names are distinct on a page") {
  for (const ParamKind kind : kKinds) {
    std::set<unsigned> seen;
    const unsigned groups = model::param_group_count(kind);
    CHECK(groups >= 1u);
    for (unsigned g = 0; g < groups; ++g) {
      CHECK(std::strlen(model::param_group_name(kind, g)) > 0u);
      const unsigned envelopes = kind == ParamKind::Instrument && g == model::instrument_param::GroupEnvelope ? model::instrument_param::kEnvelopes : 1u;
      for (unsigned e = 0; e < envelopes; ++e) {
        std::uint16_t fields[16];
        const unsigned n = model::param_group_fields(kind, g, e, fields, 16);
        CHECK(n >= 1u);
        // the parameter page's ParamView::kMaxRows; the source group is the exception (nine rows and PLAY ENGINE): the page shows the rows of the play mode, at most six
        // ... and the MOD group lists everything a destination can hold (SOURCE, the stored ENABLE, the five envelope rows and the three of the LFO: ten); the page shows six of them
        CHECK(n <= (kind == ParamKind::Instrument && (g == model::instrument_param::GroupSource || g == model::instrument_param::GroupEnvelope) ? 10u : (kind == ParamKind::Device ? 13u : 8u)));  // the Config pages hold ParamView::kConfigRows = 13 (OS-MTC B3's MTC IN; 12 with OS-MTC A3's MTC OUT and MTC START; 10 with OS-MIDICCMAP S1's CC MAP, 9 since E-CFG-03)
        std::set<std::string> names;
        for (unsigned i = 0; i < n; ++i) {
          const ParamDesc* d = model::param_desc(core::param_id(kind, 0, fields[i]));
          NEED(d != nullptr);
          CHECK(d->group == g);
          CHECK(names.insert(d->name).second);
          seen.insert(fields[i]);
        }
      }
    }
    // every field of the kind is on exactly one page
    CHECK(seen.size() == model::param_slots_per_index(kind));
  }
  CHECK(std::strlen(model::param_group_name(ParamKind::Instrument, 9)) == 0u);
  CHECK(std::string(model::envelope_name(3)) == "WT Position");
  for (unsigned e = 0; e < model::instrument_param::kEnvelopes; ++e) {
    CHECK(std::strlen(model::envelope_name(e)) <= 16u);  // the Params header: "ENVELOPE 4/6 " + the name stays clear of the page tag
  }
  CHECK(std::string(model::envelope_name(6)).empty());
  std::uint16_t small[2];
  CHECK(model::param_group_fields(ParamKind::Instrument, 0, 0, small, 2) == 2u);  // bounded by `cap`
  CHECK(model::param_group_fields(ParamKind::None, 0, 0, small, 2) == 0u);
}

TEST_CASE("param tables: storage reads and writes the value, rounds integers, refuses what is outside the range, keeps its neighbours") {
  for_each_param([](ParamId id, unsigned) {
    model::ParamField f;
    NEED(model::find_param(id, f));
    alignas(4) unsigned char owner[1024] = {};  // the biggest owner is an Instrument (704 bytes)
    const float ends[3] = {f.desc->min, f.desc->max, f.desc->def};
    for (const float v : ends) {
      CHECK_MESSAGE(model::param_store(owner, f, v), f.desc->name);
      CHECK_MESSAGE(model::param_load(owner, f) == v, f.desc->name << " " << v);
    }
    CHECK_FALSE(model::param_store(owner, f, f.desc->max + 1.0f));
    CHECK_FALSE(model::param_store(owner, f, f.desc->min - 1.0f));
    CHECK_FALSE(model::param_store(owner, f, std::nanf("")));
    CHECK(model::param_load(owner, f) == f.desc->def);  // a refused write changed nothing
    if (f.store != model::ParamStore::F32) {
      CHECK(model::param_store(owner, f, f.desc->min + 0.4f));  // nearest whole value
      CHECK(model::param_load(owner, f) == f.desc->min);
      CHECK(model::param_store(owner, f, f.desc->max - 0.4f));
      CHECK(model::param_load(owner, f) == f.desc->max);
    }
  });
  // A switch is one bit of a flag byte: setting one leaves the other.
  model::TrackSlot t{};
  model::ParamField mute, solo;
  NEED(model::find_param(core::param_id(ParamKind::Track, 3, model::track_param::Mute), mute));
  NEED(model::find_param(core::param_id(ParamKind::Track, 3, model::track_param::Solo), solo));
  CHECK(model::param_store(&t, mute, 1.0f));
  CHECK(t.flags == model::kTrackMute);
  CHECK(model::param_store(&t, solo, 1.0f));
  CHECK(t.flags == (model::kTrackMute | model::kTrackSolo));
  CHECK(model::param_store(&t, mute, 0.0f));
  CHECK(t.flags == model::kTrackSolo);
  CHECK(model::param_load(&t, mute) == 0.0f);
  CHECK(model::param_load(&t, solo) == 1.0f);
  // An envelope's fields of slot e are where the Envelope struct puts them.
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Sample);
  model::ParamField attack;
  NEED(model::find_param(core::param_id(ParamKind::Instrument, 0, model::instrument_param::envelope(4, model::instrument_param::EnvAttack)), attack));
  CHECK(model::param_store(&in, attack, 1234.0f));
  CHECK(in.sampler.envelope[4].attack == 1234);
  CHECK(in.sampler.envelope[3].attack == 3000);  // the neighbours keep the default (ours: 3 s for every envelope but the volume's)
  CHECK(in.sampler.envelope[5].attack == 3000);
}

// ---- the curves and the steps ------------------------------------------------------------------------------------------------

TEST_CASE("param curves: norm and denorm are inverses, monotonic, clamped, exact at the ends") {
  const ParamDesc* descs[] = {&kInt, &kPan, &kNorm, &kSec, &kLog, &kEnum, &kExp};
  for (const ParamDesc* d : descs) {
    CHECK_MESSAGE(core::param_desc_valid(*d), d->name);
    CHECK(core::param_norm(*d, d->min) == 0.0f);
    CHECK(core::param_norm(*d, d->max) == 1.0f);
    CHECK(core::param_denorm(*d, 0.0f) == d->min);
    CHECK(core::param_denorm(*d, 1.0f) == d->max);
    CHECK(core::param_norm(*d, d->min - 5.0f) == 0.0f);
    CHECK(core::param_norm(*d, d->max + 5.0f) == 1.0f);
    CHECK(core::param_norm(*d, std::nanf("")) == 0.0f);
    CHECK(core::param_denorm(*d, -1.0f) == d->min);
    CHECK(core::param_denorm(*d, 2.0f) == d->max);
    CHECK(core::param_denorm(*d, std::nanf("")) == d->min);
    float last = d->min;
    for (unsigned i = 0; i <= 1000; ++i) {
      const float n = static_cast<float>(i) / 1000.0f;
      const float v = core::param_denorm(*d, n);
      CHECK_MESSAGE(v >= last, d->name << " at " << i);
      last = v;
      const float back = core::param_norm(*d, v);
      CHECK_MESSAGE(std::fabs(back - n) < 2.0e-5f, d->name << " " << n << " -> " << v << " -> " << back);
    }
  }
}

TEST_CASE("param curves: log and exp against libm, and the table's knots") {
  for (unsigned i = 0; i <= 100; ++i) {
    const double n = static_cast<double>(i) / 100.0;
    const double log_expect = 20.0 * std::pow(20000.0 / 20.0, n);
    CHECK(std::fabs(static_cast<double>(core::param_denorm(kLog, static_cast<float>(n))) - log_expect) <= log_expect * 2.0e-5);
    const double exp_expect = 100.0 * (std::pow(2.0, 4.0 * n) - 1.0) / 15.0;
    CHECK(std::fabs(static_cast<double>(core::param_denorm(kExp, static_cast<float>(n))) - exp_expect) < 2.0e-3);
  }
  // The envelope time scale of the ENV page: 10 ms to the hundredth step, a second there, 100 ms a step to ten seconds.
  CHECK(core::param_denorm(kSec, 100.0f / 190.0f) == doctest::Approx(1000.0f).epsilon(1.0e-5));
  CHECK(core::param_norm(kSec, 1000.0f) == doctest::Approx(100.0f / 190.0f).epsilon(1.0e-5));
  CHECK(core::param_norm(kSec, 10.0f) == doctest::Approx(1.0f / 190.0f).epsilon(1.0e-4));
  CHECK(core::param_clamp(kInt, 99.0f) == 24.0f);
  CHECK(core::param_clamp(kInt, -99.0f) == -24.0f);
  CHECK(core::param_clamp(kInt, std::nanf("")) == 0.0f);
  CHECK(core::param_in_range(kInt, 24.0f));
  CHECK_FALSE(core::param_in_range(kInt, 24.5f));
  CHECK_FALSE(core::param_in_range(kInt, std::nanf("")));
}

TEST_CASE("param curves: a descriptor that cannot describe a value is not valid") {
  ParamDesc d = kInt;
  CHECK(core::param_desc_valid(d));
  d.max = d.min;
  CHECK_FALSE(core::param_desc_valid(d));
  d = kInt;
  d.def = 100.0f;
  CHECK_FALSE(core::param_desc_valid(d));
  d = kInt;
  d.detents = 0;
  CHECK_FALSE(core::param_desc_valid(d));
  d = kInt;
  d.coarse = 0;
  CHECK_FALSE(core::param_desc_valid(d));
  d = kInt;
  d.name = "THIRTEEN CHARS";
  CHECK_FALSE(core::param_desc_valid(d));
  d = kInt;
  d.name = "";
  CHECK_FALSE(core::param_desc_valid(d));
  d = kEnum;
  d.names = nullptr;
  CHECK_FALSE(core::param_desc_valid(d));
  d = kEnum;
  d.max = 3.0f;  // three names, four values
  CHECK_FALSE(core::param_desc_valid(d));
  d = kEnum;
  d.list_count = 4;  // four names claimed, three values
  CHECK_FALSE(core::param_desc_valid(d));
  d = kEnum;  // a minimum above 0 (a module's labelled INT, FM Algorithm 1..8): the names count from the minimum
  d.min = 1.0f;
  d.max = 3.0f;
  d.def = 1.0f;
  CHECK(core::param_desc_valid(d));
  CHECK(text(d, 1.0f) == "Low");
  CHECK(text(d, 3.0f) == "High");
  CHECK(core::param_step(d, 1.0f, 1) == 2.0f);
  float shifted = 0.0f;
  CHECK(core::param_parse(d, "high", shifted));
  CHECK(shifted == 3.0f);
  d.max = 2.0f;  // two values, three names
  CHECK_FALSE(core::param_desc_valid(d));
  d = kLog;
  d.min = 0.0f;  // a log scale from zero
  CHECK_FALSE(core::param_desc_valid(d));
  d = kSec;
  d.list_count = 1;
  CHECK_FALSE(core::param_desc_valid(d));
  static constexpr ParamKnot bad_order[] = {{0.0f, 0.0f}, {0.7f, 5000.0f}, {0.5f, 6000.0f}, {1.0f, 10000.0f}};
  d = kSec;
  d.knots = bad_order;
  d.list_count = 4;
  CHECK_FALSE(core::param_desc_valid(d));
  static constexpr ParamKnot bad_end[] = {{0.0f, 0.0f}, {1.0f, 9000.0f}};
  d = kSec;
  d.knots = bad_end;
  d.list_count = 2;
  CHECK_FALSE(core::param_desc_valid(d));
}

TEST_CASE("param steps: a detent is 1/detents of the range along the curve, ends clamp, coarse is a multiple") {
  // Linear integers: one detent is one unit; the whole range takes exactly `detents` of them.
  float v = kInt.min;
  for (int i = 0; i < 48; ++i) {
    const float next = core::param_step(kInt, v, 1);
    CHECK(next == v + 1.0f);
    v = next;
  }
  CHECK(v == kInt.max);
  CHECK(core::param_step(kInt, v, 1) == kInt.max);
  CHECK(core::param_step(kInt, kInt.min, -1) == kInt.min);
  CHECK(core::param_step(kInt, 0.0f, 12) == 12.0f);
  CHECK(core::param_step(kInt, 0.0f, -12) == -12.0f);
  CHECK(core::param_step(kInt, 0.0f, 100000) == kInt.max);
  CHECK(core::param_step(kInt, 0.0f, 0) == 0.0f);
  // The envelope time: 100 steps of 10 ms, then 90 of 100 ms: 190 detents from end to end.
  v = 0.0f;
  for (int i = 1; i <= 190; ++i) {
    v = core::param_step(kSec, v, 1);
    const float expect = i <= 100 ? 10.0f * static_cast<float>(i) : 1000.0f + 100.0f * static_cast<float>(i - 100);
    CHECK_MESSAGE(std::fabs(v - expect) < 0.05f, "detent " << i << ": " << v << " expected " << expect);
  }
  CHECK(core::param_step(kSec, v, 1) == kSec.max);
  // Off-grid values keep their offset.
  CHECK(core::param_step(kNorm, 1.234f, 1) == doctest::Approx(1.234f + 0.043f).epsilon(1.0e-4));
  // An enum moves one name a detent and stops at the ends.
  CHECK(core::param_step(kEnum, 0.0f, 1) == 1.0f);
  CHECK(core::param_step(kEnum, 2.0f, 1) == 2.0f);
  CHECK(core::param_step(kEnum, 0.0f, -1) == 0.0f);
  CHECK(core::param_step(kEnum, 0.0f, 5) == 2.0f);
}

// ---- display text -----------------------------------------------------------------------------------------------------------

TEST_CASE("param display: the numbers and names this OS shows, and the formatters the tables do not use") {
  // Our instrument page: plain integers, the pan from the centre, times in seconds with three decimals, a percentage of the range.
  CHECK(text(kInt, 86.0f) == "86");
  CHECK(text(kInt, -5.0f) == "-5");
  CHECK(text(kInt, 0.0f) == "0");
  CHECK(text(kPan, 0.0f) == "-50");
  CHECK(text(kPan, 50.0f) == "0");
  CHECK(text(kPan, 62.0f) == "12");
  CHECK(text(kPan, 100.0f) == "50");
  CHECK(text(kNorm, 0.0f) == "0");
  CHECK(text(kNorm, 4.3f) == "100");
  CHECK(text(kNorm, 2.15f) == "50");
  CHECK(text(kSec, 0.0f) == "0.000");
  CHECK(text(kSec, 10.0f) == "0.010");
  CHECK(text(kSec, 1300.0f) == "1.300");
  CHECK(text(kSec, 10000.0f) == "10.000");
  CHECK(text(kSec, 9.6f) == "0.010");
  CHECK(text(kLog, 20.0f) == "20");
  CHECK(text(kLog, 20000.0f) == "20000");
  CHECK(text(kEnum, 0.0f) == "Low");
  CHECK(text(kEnum, 2.0f) == "High");
  CHECK(text(kEnum, 7.0f) == "High");  // beyond the end: the last name
  CHECK(text(kEnum, -3.0f) == "Low");
  // The tables' own names.
  const ParamDesc* filter = model::param_desc(core::param_id(ParamKind::Instrument, 0, model::instrument_param::FilterType));
  NEED(filter != nullptr);
  CHECK(text(*filter, 0.0f) == "Low-pass");
  CHECK(text(*filter, 1.0f) == "High-pass");
  CHECK(text(*filter, 2.0f) == "Band-pass");
  const ParamDesc* limiter = model::param_desc(core::param_id(ParamKind::Project, 0, model::project_setting::LimiterMode));
  NEED(limiter != nullptr);
  CHECK(text(*limiter, 1.0f) == "Extreme");
  // A buffer too small cuts the text and still ends it.
  char tiny[4];
  CHECK(core::param_format(kSec, 10000.0f, tiny, sizeof tiny) == 3u);
  CHECK(std::strcmp(tiny, "10.") == 0);
  CHECK(core::param_format(kSec, 1.0f, tiny, 1) == 0u);
  CHECK(tiny[0] == '\0');
}

TEST_CASE("param display: parsing reads back what formatting wrote, for every row of every table and every detent") {
  unsigned checked = 0;
  for_each_param([&](ParamId id, unsigned) {
    const ParamDesc* d = model::param_desc(id);
    NEED(d != nullptr);
    std::set<std::string> shown;
    float v = d->min;
    for (unsigned k = 0; k <= d->detents; ++k) {
      const std::string s = text(*d, v);
      float parsed = 0.0f;
      CHECK_MESSAGE(core::param_parse(*d, s.c_str(), parsed), d->name << ": '" << s << "'");
      CHECK_MESSAGE(text(*d, parsed) == s, d->name << ": '" << s << "' parsed to " << parsed);
      // The unit text and spaces are accepted after the number, in any case.
      if (d->unit_text[0] != '\0') {
        float with_unit = 0.0f;
        CHECK(core::param_parse(*d, (" " + s + " " + d->unit_text + " ").c_str(), with_unit));
        CHECK(with_unit == parsed);
      }
      // Every detent shows something of its own: the user sees every step (a table whose detents are finer than its display is a bug).
      CHECK_MESSAGE(shown.insert(s).second, d->name << ": two detents show '" << s << "'");
      ++checked;
      v = core::param_step(*d, v, 1);
    }
    CHECK(v == d->max);
  });
  CHECK(checked > 3000u);
}

TEST_CASE("param display: parsing refuses what is not a value of the descriptor") {
  float out = 123.0f;
  CHECK(core::param_parse(kInt, "12", out));
  CHECK(out == 12.0f);
  CHECK(core::param_parse(kInt, "-24 st", out));
  CHECK(out == -24.0f);
  CHECK(core::param_parse(kInt, "+7", out));
  CHECK(out == 7.0f);
  out = 123.0f;
  for (const char* bad : {"", " ", "abc", "12x", "12 s", "--5", "1 2", "25", "-25", "1e3", "12.5.1", "99999999999", "0x10"}) {
    CHECK_MESSAGE(!core::param_parse(kInt, bad, out), bad);
  }
  CHECK(out == 123.0f);  // untouched on failure
  CHECK_FALSE(core::param_parse(kInt, nullptr, out));
  CHECK(core::param_parse(kSec, "0.010", out));
  CHECK(out == 10.0f);
  CHECK(core::param_parse(kSec, "1.5s", out));
  CHECK(out == 1500.0f);
  CHECK_FALSE(core::param_parse(kSec, "10.001", out));
  CHECK(core::param_parse(kLog, "1200 Hz", out));
  CHECK(out == 1200.0f);
  CHECK_FALSE(core::param_parse(kLog, "1.2k", out));  // no k: a number and its unit text only
  CHECK_FALSE(core::param_parse(kLog, "10", out));    // below 20
  CHECK(core::param_parse(kEnum, "medium", out));
  CHECK(out == 1.0f);
  CHECK(core::param_parse(kEnum, "  HIGH ", out));
  CHECK(out == 2.0f);
  CHECK_FALSE(core::param_parse(kEnum, "Med", out));
  CHECK_FALSE(core::param_parse(kEnum, "Medium rare", out));
  CHECK(core::param_parse(kPan, "-50", out));
  CHECK(out == 0.0f);
  CHECK(core::param_parse(kNorm, "100 %", out));
  CHECK(out == doctest::Approx(4.3f));
}

// ---- the settings block ----------------------------------------------------------------------------------------------------

TEST_CASE("settings: a block starts at the defaults, takes values in range, counts generations per field") {
  model::Settings s;
  std::memset(&s, 0, sizeof s);  // zeroed memory, as in the project's arena
  CHECK(s.field_count() == 0u);  // not initialised: no fields
  s.init(ParamKind::Device);
  CHECK(s.kind() == ParamKind::Device);
  CHECK(s.field_count() == model::device_setting::kCount);
  CHECK(s.is_default());
  CHECK(s.check_invariants());
  for (unsigned f = 0; f < s.field_count(); ++f) {
    const ParamDesc* d = model::param_desc(core::param_id(ParamKind::Device, 0, f));
    NEED(d != nullptr);
    CHECK(s.get(f) == d->def);
  }
  CHECK(s.get(model::device_setting::MetronomeVolume) == 50.0f);
  CHECK(s.get(model::device_setting::DisplayBrightness) == 2.0f);  // our defaults
  CHECK(s.get(99) == 0.0f);

  const std::uint32_t g_volume = s.generation(model::device_setting::MetronomeVolume);
  const std::uint32_t g_pads = s.generation(model::device_setting::PadsBrightness);
  CHECK(s.set(model::device_setting::MetronomeVolume, 80.0f) == model::Status::Ok);
  CHECK(s.get(model::device_setting::MetronomeVolume) == 80.0f);
  CHECK(s.generation(model::device_setting::MetronomeVolume) == g_volume + 1);
  CHECK(s.generation(model::device_setting::PadsBrightness) == g_pads);  // only the field that changed
  CHECK_FALSE(s.is_default());
  // The same value again: Ok, nothing changes, nothing counts.
  CHECK(s.set(model::device_setting::MetronomeVolume, 80.0f) == model::Status::Ok);
  CHECK(s.generation(model::device_setting::MetronomeVolume) == g_volume + 1);
  // Refused: out of range, NaN, no such field; nothing changes.
  CHECK(s.set(model::device_setting::MetronomeVolume, 101.0f) == model::Status::BadValue);
  CHECK(s.set(model::device_setting::MetronomeVolume, std::nanf("")) == model::Status::BadValue);
  CHECK(s.set(model::device_setting::ClockSync, 7.0f) == model::Status::BadValue);
  CHECK(s.set(model::device_setting::ClockSync, -6.0f) == model::Status::Ok);
  CHECK(s.get(model::device_setting::ClockSync) == -6.0f);  // a signed field
  CHECK(s.set(31, 0.0f) == model::Status::BadIndex);  // 31 is past the last field (OS-LEDBPM took 30, OS-MTC B3 29, A3 27 and 28, OS-MIDICCMAP S1 26; the block's room is 32)
  CHECK(s.get(model::device_setting::MetronomeVolume) == 80.0f);
  CHECK(s.generation(model::device_setting::MetronomeVolume) == g_volume + 1);
  CHECK(s.check_invariants());

  // init() again: every generation moves (none repeats, whatever it was), the values are the defaults.
  std::uint32_t before[model::device_setting::kCount];
  for (unsigned f = 0; f < s.field_count(); ++f) {
    before[f] = s.generation(f);
  }
  s.init(ParamKind::Device);
  CHECK(s.is_default());
  for (unsigned f = 0; f < s.field_count(); ++f) {
    CHECK(s.generation(f) != before[f]);
  }
}

TEST_CASE("settings: the two scopes have their own fields, and a block is assigned whole or not at all") {
  model::Settings project;
  std::memset(&project, 0, sizeof project);
  project.init(ParamKind::Project);
  CHECK(project.field_count() == model::project_setting::kCount);
  CHECK(project.get(model::project_setting::AntiAliasing) == 1.0f);  // our default: on
  CHECK(project.get(model::project_setting::SendsMode) == 0.0f);     // pre-fader
  model::Settings device;
  std::memset(&device, 0, sizeof device);
  device.init(ParamKind::Device);
  CHECK(project.assign(device) == model::Status::BadValue);  // another scope
  model::Settings other;
  std::memset(&other, 0, sizeof other);
  other.init(ParamKind::Project);
  CHECK(other.set(model::project_setting::SendsMode, 1.0f) == model::Status::Ok);
  const std::uint32_t g_sends = project.generation(model::project_setting::SendsMode);
  const std::uint32_t g_limiter = project.generation(model::project_setting::LimiterMode);
  CHECK(project.assign(other) == model::Status::Ok);
  CHECK(project.get(model::project_setting::SendsMode) == 1.0f);
  CHECK(project.generation(model::project_setting::SendsMode) == g_sends + 1);
  CHECK(project.generation(model::project_setting::LimiterMode) == g_limiter);  // what did not differ did not count
  CHECK(project.assign(other) == model::Status::Ok);
  CHECK(project.generation(model::project_setting::SendsMode) == g_sends + 1);  // and assigning what is there counts nothing
  // An invalid block is refused whole: a binding of the wrong scope (a mixer parameter belongs to the device's store).
  CHECK(other.set_learn(0, core::LearnBinding{core::param_id(ParamKind::Track, 0, 0), 7, 0, 0, 0}) == model::Status::BadValue);
  CHECK(project.set_learn(0, core::LearnBinding{core::param_id(ParamKind::Instrument, 0, 7), 74, 0, 0, 0}) == model::Status::Ok);
  CHECK(device.set_learn(0, core::LearnBinding{core::param_id(ParamKind::Track, 2, 0), 7, 0, 0, 0}) == model::Status::Ok);
  CHECK(device.set_learn(1, core::LearnBinding{core::param_id(ParamKind::Instrument, 0, 7), 74, 0, 0, 0}) == model::Status::BadValue);
  CHECK(project.set_learn(core::kLearnSlots, core::LearnBinding{}) == model::Status::BadIndex);
  CHECK(project.learn(core::kLearnSlots).param == ParamId::None);  // out of range reads as empty
  CHECK(project.learn_count() == 1u);
  CHECK_FALSE(project.is_default());
  CHECK(project.check_invariants());
}

// ---- MIDI Learn: the data and the rules -----------------------------------------------------------------------------------

namespace {

const ParamId kCutoff = core::param_id(ParamKind::Instrument, 0, 7);
const ParamId kResonance = core::param_id(ParamKind::Instrument, 0, 8);
const ParamId kVolume = core::param_id(ParamKind::Instrument, 0, 0);

core::LearnBinding bind(ParamId p, unsigned cc, unsigned channel = 0, core::LearnMode mode = core::LearnMode::Pickup, unsigned flags = 0) {
  return core::LearnBinding{p, static_cast<std::uint8_t>(cc), static_cast<std::uint8_t>(channel), static_cast<std::uint8_t>(mode),
                            static_cast<std::uint8_t>(flags)};
}

}  // namespace

TEST_CASE("learn: scopes follow the maintainer's split, bindings are valid or empty, a store keeps one controller to one parameter") {
  CHECK(core::learn_scope(ParamKind::Instrument) == core::LearnScope::Project);
  CHECK(core::learn_scope(ParamKind::Project) == core::LearnScope::Project);
  CHECK(core::learn_scope(ParamKind::Track) == core::LearnScope::Device);
  CHECK(core::learn_scope(ParamKind::Master) == core::LearnScope::Device);
  CHECK(core::learn_scope(ParamKind::Device) == core::LearnScope::Device);
  CHECK(sizeof(core::LearnBinding) == 8u);

  CHECK(core::learn_valid(core::LearnBinding{}));
  CHECK(core::learn_valid(bind(kCutoff, 74)));
  CHECK(core::learn_valid(bind(kCutoff, 127, 16, core::LearnMode::Relative)));
  CHECK_FALSE(core::learn_valid(bind(kCutoff, 128)));
  CHECK_FALSE(core::learn_valid(bind(kCutoff, 1, 17)));
  CHECK_FALSE(core::learn_valid(bind(kCutoff, 1, 0, static_cast<core::LearnMode>(3))));
  CHECK_FALSE(core::learn_valid(bind(kCutoff, 1, 0, core::LearnMode::Pickup, 2)));
  CHECK_FALSE(core::learn_valid(bind(ParamId::None, 5)));  // an empty slot is all zero: a cc without a parameter is not empty
  CHECK_FALSE(core::learn_valid(bind(core::param_id(ParamKind::None, 0, 1), 5)));
  CHECK(core::learn_valid(bind(core::param_id(ParamKind::Instrument, 0, 7), 5, 0, core::LearnMode::Pickup, core::kLearnFollow)));
  CHECK_FALSE(core::learn_valid(bind(core::param_id(ParamKind::Instrument, 3, 7), 5, 0, core::LearnMode::Pickup, core::kLearnFollow)));  // follow has no index
  CHECK_FALSE(core::learn_valid(bind(core::param_id(ParamKind::Track, 0, 7), 5, 0, core::LearnMode::Pickup, core::kLearnFollow)));

  // targets: the exact parameter, or the same field of any instrument for a follow binding
  CHECK(core::learn_targets(bind(kCutoff, 74), kCutoff));
  CHECK_FALSE(core::learn_targets(bind(kCutoff, 74), core::param_id(ParamKind::Instrument, 1, 7)));
  const core::LearnBinding follow = bind(kCutoff, 74, 0, core::LearnMode::Pickup, core::kLearnFollow);
  CHECK(core::learn_targets(follow, core::param_id(ParamKind::Instrument, 9, 7)));
  CHECK_FALSE(core::learn_targets(follow, core::param_id(ParamKind::Instrument, 9, 8)));
  CHECK_FALSE(core::learn_targets(core::LearnBinding{}, ParamId::None));

  // overlap: the same controller on channels that meet (0 meets every one)
  CHECK(core::learn_overlap(bind(kCutoff, 74), bind(kResonance, 74)));
  CHECK(core::learn_overlap(bind(kCutoff, 74, 3), bind(kResonance, 74, 0)));
  CHECK_FALSE(core::learn_overlap(bind(kCutoff, 74, 3), bind(kResonance, 74, 4)));
  CHECK_FALSE(core::learn_overlap(bind(kCutoff, 74), bind(kResonance, 75)));
  CHECK_FALSE(core::learn_overlap(core::LearnBinding{}, core::LearnBinding{}));
}

TEST_CASE("learn: an insert's parameter (OS-INSFX S7) is a project binding with an id of this version, and never follows") {
  const auto ins = [](unsigned index, unsigned field) { return core::param_id(ParamKind::Insert, index, field); };
  CHECK(core::learn_scope(ParamKind::Insert) == core::LearnScope::Project);
  CHECK(core::learn_valid(bind(ins(0, 0), 20)));
  CHECK(core::learn_valid(bind(ins(31, 31), 20, 16, core::LearnMode::Jump)));                 // track 16, INS B, the last field
  CHECK(core::learn_valid(bind(ins(core::kInsertIdMaster, 5), 20)));                           // the master insert: index 32
  CHECK_FALSE(core::learn_valid(bind(ins(core::kInsertIdMaster + 1, 0), 20)));                 // the master has no slot 1
  CHECK_FALSE(core::learn_valid(bind(ins(0, core::kInsertIdFields), 20)));                     // field 32 is past O3M_PARAMS_MAX
  CHECK_FALSE(core::learn_valid(bind(ins(0, 0), 20, 0, core::LearnMode::Pickup, core::kLearnFollow)));
  CHECK(core::insert_param_valid(ins(32, 31)));
  CHECK_FALSE(core::insert_param_valid(ins(33, 0)));
  CHECK_FALSE(core::insert_param_valid(core::param_id(ParamKind::Instrument, 0, 0)));
  // targets and planning are the same as for any binding: one controller moves one parameter
  CHECK(core::learn_targets(bind(ins(5, 3), 20), ins(5, 3)));
  CHECK_FALSE(core::learn_targets(bind(ins(5, 3), 20), ins(4, 3)));
  core::LearnBinding store[core::kLearnSlots] = {};
  core::LearnPlan plan{0, 0};
  CHECK(core::learn_plan(store, core::kLearnSlots, bind(ins(5, 3), 20), plan));
  store[plan.write] = bind(ins(5, 3), 20);
  CHECK(core::learn_plan(store, core::kLearnSlots, bind(ins(5, 3), 21), plan));  // the same parameter again: its own slot
  CHECK(plan.write == 0);
  CHECK(core::learn_plan(store, core::kLearnSlots, bind(ins(6, 0), 20), plan));  // another parameter on the same controller takes the message over
  CHECK(plan.write == 0);
  CHECK(core::learn_find_param(store, core::kLearnSlots, ins(5, 3)) == 0);
  CHECK(core::learn_store_valid(store, core::kLearnSlots));
}

TEST_CASE("learn: planning a binding into a store") {
  core::LearnBinding store[core::kLearnSlots] = {};
  core::LearnPlan plan{0, 0};
  CHECK(core::learn_plan(store, core::kLearnSlots, bind(kCutoff, 74), plan));
  CHECK((plan.write == 0 && plan.clear == 0u));  // an empty store: the first slot
  store[0] = bind(kCutoff, 74);
  CHECK(core::learn_plan(store, core::kLearnSlots, bind(kResonance, 71), plan));
  CHECK((plan.write == 1 && plan.clear == 0u));
  store[1] = bind(kResonance, 71, 2);
  // The same parameter again keeps its slot (and may change controller, channel and mode).
  CHECK(core::learn_plan(store, core::kLearnSlots, bind(kCutoff, 20, 5, core::LearnMode::Jump), plan));
  CHECK((plan.write == 0 && plan.clear == 0u));
  // A new parameter on a controller that is taken displaces the old owner and takes its slot.
  CHECK(core::learn_plan(store, core::kLearnSlots, bind(kVolume, 74), plan));
  CHECK((plan.write == 0 && plan.clear == 0u));
  // The same parameter moving to a controller another one has: the other one is emptied.
  CHECK(core::learn_plan(store, core::kLearnSlots, bind(kCutoff, 71), plan));
  CHECK(plan.write == 0);
  CHECK(plan.clear == 2u);  // slot 1
  // A binding on every channel displaces every binding of the controller on a channel: several slots at once.
  store[2] = bind(kVolume, 71, 3);
  CHECK(core::learn_plan(store, core::kLearnSlots, bind(core::param_id(ParamKind::Instrument, 0, 9), 71, 0), plan));
  CHECK(plan.write == 1);  // the first of the three that listen to 71 (slots 1 and 2)
  CHECK(plan.clear == (1u << 2));
  // Invalid and empty bindings are not planned.
  CHECK_FALSE(core::learn_plan(store, core::kLearnSlots, bind(kCutoff, 200), plan));
  CHECK_FALSE(core::learn_plan(store, core::kLearnSlots, core::LearnBinding{}, plan));
  // A full store takes no new parameter on a free controller, but still the same parameter again and a displacing one.
  core::LearnBinding full[core::kLearnSlots];
  for (unsigned i = 0; i < core::kLearnSlots; ++i) {
    full[i] = bind(core::param_id(ParamKind::Instrument, i, 7), i);
  }
  CHECK(core::learn_store_valid(full, core::kLearnSlots));
  CHECK_FALSE(core::learn_plan(full, core::kLearnSlots, bind(core::param_id(ParamKind::Instrument, 40, 7), 100), plan));
  CHECK(core::learn_plan(full, core::kLearnSlots, bind(core::param_id(ParamKind::Instrument, 5, 7), 100), plan));
  CHECK(plan.write == 5);
  CHECK(core::learn_plan(full, core::kLearnSlots, bind(core::param_id(ParamKind::Instrument, 40, 7), 9), plan));
  CHECK(plan.write == 9);
  // The lookups.
  CHECK(core::learn_find_param(full, core::kLearnSlots, core::param_id(ParamKind::Instrument, 17, 7)) == 17);
  CHECK(core::learn_find_param(full, core::kLearnSlots, core::param_id(ParamKind::Instrument, 17, 8)) == -1);
  CHECK(core::learn_find_cc(full, core::kLearnSlots, 1, 31) == 31);
  CHECK(core::learn_find_cc(full, core::kLearnSlots, 1, 31, 32) == -1);
  CHECK(core::learn_find_cc(full, core::kLearnSlots, 0, 99) == -1);
  full[3].channel = 5;
  CHECK(core::learn_find_cc(full, core::kLearnSlots, 5, 3) == 3);
  CHECK(core::learn_find_cc(full, core::kLearnSlots, 6, 3) == -1);
  CHECK(core::learn_find_cc(full, core::kLearnSlots, 0, 3) == 3);  // a message of no channel matches any binding
  // Validity of a whole store: no parameter twice, no two that listen alike.
  full[4] = full[3];
  CHECK_FALSE(core::learn_store_valid(full, core::kLearnSlots));
  full[4] = bind(core::param_id(ParamKind::Instrument, 4, 7), 3, 6);
  CHECK(core::learn_store_valid(full, core::kLearnSlots));  // the same controller on another channel is fine
  full[4].param = full[3].param;
  full[4].cc = 77;
  CHECK_FALSE(core::learn_store_valid(full, core::kLearnSlots));  // the same parameter twice
}

namespace {
const ParamId kTarget = core::param_id(ParamKind::Instrument, 3, 7);
}

TEST_CASE("learn: pickup waits for the knob to reach the parameter, never jumps, and catches again when the parameter was moved") {
  core::LearnRuntime rt{};
  float out = -1.0f;
  const float at_half = 0.5f;  // the parameter is at half (63.5 on the knob's 0..127)
  // Below it: ignored until the knob gets within one step.
  CHECK_FALSE(core::learn_absolute(rt, kTarget, core::LearnMode::Pickup, 0, at_half, 1, out));
  CHECK_FALSE(core::learn_absolute(rt, kTarget, core::LearnMode::Pickup, 30, at_half, 1, out));
  CHECK_FALSE(core::learn_absolute(rt, kTarget, core::LearnMode::Pickup, 61, at_half, 1, out));
  CHECK_FALSE(core::learn_absolute(rt, kTarget, core::LearnMode::Pickup, 62, at_half, 1, out));  // 1.5 away: not yet
  CHECK(out == -1.0f);  // nothing was written
  CHECK(core::learn_absolute(rt, kTarget, core::LearnMode::Pickup, 63, at_half, 1, out));  // within one of 63.5
  CHECK(out == doctest::Approx(63.0f / 127.0f));
  // From then on the knob drives it, whatever the distance.
  CHECK(core::learn_absolute(rt, kTarget, core::LearnMode::Pickup, 100, 63.0f / 127.0f, 1, out));
  CHECK(out == doctest::Approx(100.0f / 127.0f));
  CHECK(core::learn_absolute(rt, kTarget, core::LearnMode::Pickup, 0, 100.0f / 127.0f, 1, out));
  CHECK(out == 0.0f);
  // Something else moved the parameter (its generation changed): the knob has to catch it again.
  core::learn_applied(rt, kTarget, 1);
  CHECK(core::learn_absolute(rt, kTarget, core::LearnMode::Pickup, 5, 0.0f, 1, out));  // same generation: still caught
  CHECK_FALSE(core::learn_absolute(rt, kTarget, core::LearnMode::Pickup, 5, 0.9f, 2, out));  // generation 2: not caught, the parameter is at 0.9
  CHECK_FALSE(core::learn_absolute(rt, kTarget, core::LearnMode::Pickup, 20, 0.9f, 2, out));
  CHECK(core::learn_absolute(rt, kTarget, core::LearnMode::Pickup, 114, 0.9f, 2, out));  // 0.9 * 127 = 114.3
  // Crossing: the knob went past the parameter between two messages.
  core::LearnRuntime r2{};
  CHECK_FALSE(core::learn_absolute(r2, kTarget, core::LearnMode::Pickup, 10, 0.5f, 7, out));
  CHECK(core::learn_absolute(r2, kTarget, core::LearnMode::Pickup, 120, 0.5f, 7, out));  // 10 -> 120 passed 63.5
  CHECK(out == doctest::Approx(120.0f / 127.0f));
  core::LearnRuntime r3{};
  CHECK_FALSE(core::learn_absolute(r3, kTarget, core::LearnMode::Pickup, 120, 0.5f, 7, out));
  CHECK_FALSE(core::learn_absolute(r3, kTarget, core::LearnMode::Pickup, 90, 0.5f, 7, out));  // coming down, not there yet
  CHECK(core::learn_absolute(r3, kTarget, core::LearnMode::Pickup, 30, 0.5f, 7, out));  // 90 -> 30 crossed it
  // Jump takes the knob's position at once; a value above 127 is held to 127.
  core::LearnRuntime r4{};
  CHECK(core::learn_absolute(r4, kTarget, core::LearnMode::Jump, 127, 0.0f, 3, out));
  CHECK(out == 1.0f);
  CHECK(core::learn_absolute(r4, kTarget, core::LearnMode::Jump, 200, 0.0f, 3, out));
  CHECK(out == 1.0f);
  CHECK(core::learn_absolute(r4, kTarget, core::LearnMode::Jump, 0, 1.0f, 3, out));
  CHECK(out == 0.0f);
  // Relative: the common two's complement encoding.
  CHECK(core::learn_relative_delta(1) == 1);
  CHECK(core::learn_relative_delta(63) == 63);
  CHECK(core::learn_relative_delta(64) == 0);
  CHECK(core::learn_relative_delta(0) == 0);
  CHECK(core::learn_relative_delta(65) == -63);
  CHECK(core::learn_relative_delta(127) == -1);
  CHECK(core::learn_relative_delta(128) == 0);
}

TEST_CASE("learn: an edit made elsewhere is never crossed by a knob that was last seen before it, and another parameter starts afresh") {
  core::LearnRuntime rt{};
  float out = -1.0f;
  const float p11 = 11.0f / 127.0f;
  // The knob has caught the parameter at 11 and written it.
  CHECK(core::learn_absolute(rt, kTarget, core::LearnMode::Pickup, 11, p11, 1, out));
  core::learn_applied(rt, kTarget, 2);
  out = -1.0f;
  // An edit on the screen takes it to 76 (generation 3). The knob, still at 11, goes to 110: it is 34 away from the parameter, and
  // the 11 it was last seen at is from before the edit, so the move from 11 to 110 does not count as passing 76.
  const float p76 = 76.0f / 127.0f;
  CHECK_FALSE(core::learn_absolute(rt, kTarget, core::LearnMode::Pickup, 110, p76, 3, out));
  CHECK(out == -1.0f);  // no jump
  // Moving on from 110 down through 76 does catch it.
  CHECK(core::learn_absolute(rt, kTarget, core::LearnMode::Pickup, 70, p76, 3, out));
  CHECK(out == doctest::Approx(70.0f / 127.0f));

  // The same state at the same generation, but a different parameter (a follow binding after the selection moved): afresh, not caught.
  const ParamId other = core::param_id(ParamKind::Instrument, 4, 7);
  CHECK_FALSE(core::learn_absolute(rt, other, core::LearnMode::Pickup, 100, 0.2f, 3, out));
  CHECK_FALSE(core::learn_absolute(rt, other, core::LearnMode::Pickup, 120, 0.2f, 3, out));  // one message of history: nothing crossed
  CHECK(core::learn_absolute(rt, other, core::LearnMode::Pickup, 25, 0.2f, 3, out));          // 120 -> 25 passed 25.4
  CHECK(core::learn_key(kTarget) != core::learn_key(other));
  CHECK(core::learn_key(core::param_id(ParamKind::Instrument, 3, 8)) != core::learn_key(kTarget));
}

// ---- the ring to the audio side, under load -------------------------------------------------------------------------------

namespace {

// The audio side as a test sees it: what it last heard for each parameter (a small open table), and how many messages it took.
struct FakeAudio {
  static constexpr unsigned kSlots = 128;
  std::uint32_t id[kSlots] = {};
  float value[kSlots] = {};
  unsigned used = 0;
  std::uint32_t heard = 0;

  void apply(const core::ParamChange& c) {
    ++heard;
    for (unsigned i = 0; i < used; ++i) {
      if (id[i] == static_cast<std::uint32_t>(c.id)) {
        value[i] = c.value;
        return;
      }
    }
    if (used < kSlots) {
      id[used] = static_cast<std::uint32_t>(c.id);
      value[used++] = c.value;
    }
  }
  bool has(ParamId p, float& v) const {
    for (unsigned i = 0; i < used; ++i) {
      if (id[i] == static_cast<std::uint32_t>(p)) {
        v = value[i];
        return true;
      }
    }
    return false;
  }
  // One block: at most kMaxParamsPerBlock messages, in order (the engine's drain).
  unsigned drain(core::ParamRing& ring) {
    unsigned n = 0;
    core::ParamChange c;
    while (n < core::kMaxParamsPerBlock && ring.pop(c)) {
      apply(c);
      ++n;
    }
    return n;
  }
};

core::ParamRing& ring() {
  static core::ParamRing r;
  return r;
}

std::uint32_t next(std::uint32_t& s) {
  s = s * 1664525u + 1013904223u;
  return s >> 8;
}

}  // namespace

TEST_CASE("param ring: messages arrive in order, a block takes at most 64, a full ring refuses and counts") {
  core::ParamRing& r = ring();
  FakeAudio audio;
  core::ParamChange scrap;
  while (r.pop(scrap)) {
  }
  for (std::uint32_t i = 0; i < core::kParamRingSize; ++i) {
    CHECK(r.push(core::ParamChange{core::param_id(ParamKind::Instrument, 0, i % 13), static_cast<float>(i), 0}));
  }
  CHECK_FALSE(r.push(core::ParamChange{kCutoff, 1.0f, 0}));  // full: the producer is told, never blocked
  CHECK(audio.drain(r) == 64u);
  CHECK(audio.heard == 64u);
  CHECK(audio.drain(r) == 64u);
  CHECK(audio.drain(r) == 64u);
  CHECK(audio.drain(r) == 64u);
  CHECK(audio.drain(r) == 0u);
  CHECK(audio.heard == core::kParamRingSize);
  // In order: the last write to each of the 13 parameters is the last one that was pushed for it.
  for (unsigned f = 0; f < 13; ++f) {
    float v = -1.0f;
    NEED(audio.has(core::param_id(ParamKind::Instrument, 0, f), v));
    unsigned last = 0;
    for (unsigned i = 0; i < core::kParamRingSize; ++i) {
      last = i % 13 == f ? i : last;
    }
    CHECK(v == static_cast<float>(last));
  }
}

TEST_CASE("param ring: under load the audio side ends where the producer is, once the producer resends what was refused") {
  core::ParamRing& r = ring();
  core::ParamChange scrap;
  while (r.pop(scrap)) {
  }
  FakeAudio audio;
  constexpr unsigned kParams = 40;
  float truth[kParams] = {};     // what the main loop believes each parameter is
  bool refused[kParams] = {};    // pushes the ring did not take: the audio side is behind on these
  std::uint32_t seed = 12345u;
  std::uint32_t pushed = 0;
  std::uint32_t dropped = 0;
  for (unsigned block = 0; block < 4000 / kScale; ++block) {
    // Bursts: now nothing, now a flood far over one block's drain (a MIDI controller sweep, a macro).
    const unsigned burst = block % 50 == 7 ? 400u : next(seed) % 20u;
    for (unsigned i = 0; i < burst; ++i) {
      const unsigned p = next(seed) % kParams;
      const float v = static_cast<float>(next(seed) % 1000u) * 0.001f;
      truth[p] = v;
      if (r.push(core::ParamChange{core::param_id(ParamKind::Instrument, p / 13, p % 13), v, 0})) {
        ++pushed;
        refused[p] = false;
      } else {
        ++dropped;
        refused[p] = true;
      }
    }
    // The producer resends the current value of what was refused, once there is room (it knows from the return value).
    for (unsigned p = 0; p < kParams; ++p) {
      if (refused[p] && r.push(core::ParamChange{core::param_id(ParamKind::Instrument, p / 13, p % 13), truth[p], 0})) {
        refused[p] = false;
        ++pushed;
      }
    }
    CHECK(audio.drain(r) <= core::kMaxParamsPerBlock);
  }
  while (audio.drain(r) != 0) {
  }
  CHECK(dropped > 0u);           // the load really did overrun the ring
  CHECK(pushed > 10000u / kScale);
  CHECK(audio.heard == pushed);  // every accepted message was heard exactly once
  for (unsigned p = 0; p < kParams; ++p) {
    float v = -1.0f;
    if (audio.has(core::param_id(ParamKind::Instrument, p / 13, p % 13), v)) {
      CHECK_MESSAGE(v == truth[p], "parameter " << p);
    } else {
      CHECK(truth[p] == 0.0f);  // never touched
    }
  }
}
