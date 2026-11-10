// SPDX-License-Identifier: GPL-3.0-or-later
// OS-PARAMAUTO S1 (docs/specs/OS-PARAMAUTO.md sections 3 and 8): the CC letters of a Sample or Module instrument. The six midi_cc bytes of a non-MIDI instrument are parameter codes
// (Sample: field + 1 of a learnable field 0x00..0x77; Module: parameter index + 1); the code and the id decode each other, the project-wide map takes the lowest slot's letter, and the
// file says it with INST 1.9 (the 1.8 layout whole, no new byte), written only for a non-MIDI instrument with a letter; an older chunk reads the six bytes as 0.
#include <doctest/doctest.h>

#include "support/iff.h"
#include "support/memory_io.h"
#include "support/need.h"
#include "support/project_fixture.h"

#include <o3os/core/param.h>
#include <o3os/formats/crc32.h>
#include <o3os/formats/native.h>
#include <o3os/model/instrument.h>
#include <o3os/model/param_auto.h>
#include <o3os/model/params.h>
#include <o3os/model/project.h>

#include <cstdint>
#include <vector>

using namespace o3os;
using namespace o3os::formats::native;
using namespace o3os::model;
using o3os::core::ParamId;
using o3os::core::ParamKind;
using o3os::test::RawChunk;
using Bytes = std::vector<std::uint8_t>;

namespace {

Project& proj(int i = 0) { return test::arena(i).project; }

constexpr std::uint32_t kSampleSlot = 0;  // the small project's Sample instrument
constexpr std::uint32_t kMidiSlot = 20;   // its MIDI instrument
constexpr std::uint32_t kModuleSlot = 50; // its Module instrument

// The small project plus a MIDI instrument in slot kMidiSlot (the small fixture has none): its midi_cc bytes are controller numbers.
void fill_with_midi(Project& p) {
  test::fill_small(p);
  Instrument midi;
  init_instrument(midi, InstrumentKind::Midi);
  midi.midi_channel = 5;
  midi.midi_cc[0] = 11;
  NEED(p.set_instrument(kMidiSlot, midi) == Status::Ok);
}

Bytes saved(const Project& p) {
  Bytes out;
  CHECK(test::save(p, out) == Progress::Done);
  return out;
}

RawChunk* inst_of(std::vector<RawChunk>& cs, std::uint32_t slot) {
  for (RawChunk& c : cs) {
    if (c.id == kChunkInst && c.payload.size() > 2 && c.payload[2] == slot) {
      return &c;
    }
  }
  return nullptr;
}

std::uint16_t inst_version(const Bytes& file, std::uint32_t slot) {
  std::vector<RawChunk> cs = test::split_chunks(file);
  RawChunk* c = inst_of(cs, slot);
  CHECK(c != nullptr);
  if (c == nullptr) {
    return 0;
  }
  return static_cast<std::uint16_t>(c->payload[0] << 8 | c->payload[1]);
}

std::size_t inst_size(const Bytes& file, std::uint32_t slot) {
  std::vector<RawChunk> cs = test::split_chunks(file);
  RawChunk* c = inst_of(cs, slot);
  CHECK(c != nullptr);
  if (c == nullptr) {
    return 0;
  }
  return c->payload.size();
}

void set_letters(Project& p, std::uint32_t slot, const std::uint8_t (&codes)[6]) {
  Instrument in = *p.instrument(slot);
  for (unsigned i = 0; i < 6; ++i) {
    in.midi_cc[i] = codes[i];
  }
  CHECK(p.set_instrument(slot, in) == Status::Ok);
}

ParamId sample_id(std::uint32_t slot, unsigned field) { return core::param_id(ParamKind::Instrument, slot, field); }

}  // namespace

TEST_CASE("param auto: a learnable Sample field is coded field + 1 and decodes back; the rest is not codable") {
  unsigned coded = 0;
  for (unsigned f = 0; f < kAutoSampleFields; ++f) {
    const ParamId id = sample_id(7, f);
    const core::ParamDesc* d = param_desc(id);
    const std::uint8_t code = auto_code(id);
    if (d != nullptr && (d->flags & core::kParamLearnable) != 0) {
      CHECK(code == f + 1u);
      CHECK(auto_param(7, InstrumentKind::Sample, code) == id);
      ++coded;
    } else {
      CHECK(code == 0);
    }
  }
  CHECK(coded > 20);  // the sampler rows are learnable (cutoff, resonance, sends, LFO and envelope rows ...)
  CHECK(auto_code(sample_id(7, instrument_param::Volume)) == 1);
  CHECK(auto_code(sample_id(7, instrument_param::Cutoff)) == instrument_param::Cutoff + 1);
  // VEL>, the wavetable rows and every field past the sampler's: no FX of their own and no letter either
  CHECK(auto_code(sample_id(7, instrument_param::VelSens)) == 0);
  CHECK(auto_code(sample_id(7, instrument_param::kWtFirst)) == 0);
  CHECK(auto_code(sample_id(7, 0x77 + 1)) == 0);
  // another kind never has a letter
  CHECK(auto_code(core::param_id(ParamKind::Track, 3, track_param::Volume)) == 0);
  CHECK(auto_code(core::param_id(ParamKind::Master, 0, master_param::Volume)) == 0);
  CHECK(auto_code(ParamId::None) == 0);
}

TEST_CASE("param auto: the 32 Module parameters are coded index + 1 and decode back; one past is not codable") {
  for (unsigned i = 0; i < instrument_param::kModuleMax; ++i) {
    const ParamId id = sample_id(9, instrument_param::kModuleFirst + i);
    CHECK(auto_code(id) == i + 1u);
    CHECK(auto_param(9, InstrumentKind::Module, static_cast<std::uint8_t>(i + 1u)) == id);
  }
  CHECK(auto_code(sample_id(9, instrument_param::kModuleFirst + instrument_param::kModuleMax)) == 0);
  CHECK(auto_param(9, InstrumentKind::Module, instrument_param::kModuleMax + 1u) == ParamId::None);
}

TEST_CASE("param auto: a code that does not fit its kind decodes to None, and a MIDI instrument has no letters") {
  CHECK(auto_param(1, InstrumentKind::Sample, 0) == ParamId::None);
  CHECK(auto_param(1, InstrumentKind::Sample, kAutoSampleFields + 1u) == ParamId::None);
  CHECK(auto_param(1, InstrumentKind::Sample, 255) == ParamId::None);
  CHECK(auto_param(1, InstrumentKind::Midi, 1) == ParamId::None);
  Instrument in;
  init_instrument(in, InstrumentKind::Midi);
  in.midi_cc[2] = 5;
  CHECK_FALSE(inst_has_auto_letter(in));
  init_instrument(in, InstrumentKind::Sample);
  CHECK_FALSE(inst_has_auto_letter(in));
  in.midi_cc[5] = 1;
  CHECK(inst_has_auto_letter(in));
  init_instrument(in, InstrumentKind::Module);
  in.midi_cc[0] = 1;
  CHECK(inst_has_auto_letter(in));
}

TEST_CASE("param auto: the project map takes the lowest slot's letter, skips MIDI instruments, and first_free counts from 0 to 6 taken") {
  test::fill_small(proj(0));
  AutoMap m;
  auto_scan(proj(0), m);
  CHECK(auto_first_free(m) == 0);
  for (unsigned l = 0; l < kAutoLetters; ++l) {
    CHECK(m.id[l] == ParamId::None);
  }
  // letters A and C on the Sample instrument, C and F on the Module one: C is the Sample's (the lower slot), the Module's duplicate is ignored
  const std::uint8_t sample_codes[6] = {instrument_param::Volume + 1, 0, instrument_param::Cutoff + 1, 0, 0, 0};
  const std::uint8_t module_codes[6] = {0, 0, 4, 0, 0, 2};
  set_letters(proj(0), kSampleSlot, sample_codes);
  set_letters(proj(0), kModuleSlot, module_codes);
  auto_scan(proj(0), m);
  CHECK(m.id[0] == sample_id(kSampleSlot, instrument_param::Volume));
  CHECK(m.id[1] == ParamId::None);
  CHECK(m.id[2] == sample_id(kSampleSlot, instrument_param::Cutoff));
  CHECK(m.id[5] == sample_id(kModuleSlot, instrument_param::kModuleFirst + 1));
  CHECK(auto_find(m, sample_id(kSampleSlot, instrument_param::Cutoff)) == 2);
  CHECK(auto_find(m, sample_id(kModuleSlot, instrument_param::kModuleFirst + 3)) == -1);  // the ignored duplicate
  CHECK(auto_find(m, ParamId::None) == -1);
  CHECK(auto_first_free(m) == 1);
  // a letter on a MIDI instrument is a controller number, never a letter
  Instrument midi;
  init_instrument(midi, InstrumentKind::Midi);
  midi.midi_channel = 5;
  midi.midi_cc[1] = 7;
  NEED(proj(0).set_instrument(kMidiSlot, midi) == Status::Ok);
  auto_scan(proj(0), m);
  CHECK(m.id[1] == ParamId::None);
  // six taken
  const std::uint8_t all[6] = {1, 2, 3, 4, 5, 6};
  set_letters(proj(0), kSampleSlot, all);
  auto_scan(proj(0), m);
  CHECK(auto_first_free(m) == -1);
  for (unsigned l = 0; l < kAutoLetters; ++l) {
    CHECK(m.id[l] == sample_id(kSampleSlot, l));
  }
}

TEST_CASE("param auto: a project without letters is the file it was; with letters the instrument is INST 1.9 of the same size, and a MIDI instrument stays 1.3") {
  fill_with_midi(proj(0));
  const Bytes plain = saved(proj(0));
  CHECK(inst_version(plain, kSampleSlot) < 0x0109);
  CHECK(inst_version(plain, kModuleSlot) < 0x0109);
  const std::size_t sample_size = inst_size(plain, kSampleSlot);
  const std::size_t module_size = inst_size(plain, kModuleSlot);
  const std::uint8_t sample_codes[6] = {instrument_param::Volume + 1, 0, instrument_param::Cutoff + 1, 0, 0, 0};
  const std::uint8_t module_codes[6] = {0, 3, 0, 0, 0, 0};
  set_letters(proj(0), kSampleSlot, sample_codes);
  set_letters(proj(0), kModuleSlot, module_codes);
  const Bytes file = saved(proj(0));
  CHECK(inst_version(file, kSampleSlot) == kInstVersionAutoCc);
  CHECK(inst_version(file, kModuleSlot) == kInstVersionAutoCc);
  CHECK(inst_version(file, kMidiSlot) == inst_version(plain, kMidiSlot));
  // no new byte: 1.9 holds the 1.8 layout whole (the writer's gates are all "version >= x", so every field the instrument had is still there)
  CHECK(inst_size(file, kSampleSlot) >= sample_size);
  CHECK(inst_size(file, kModuleSlot) >= module_size);
  // every chunk but those two instruments, the CRC aside, is the plain file's
  std::vector<RawChunk> a = test::split_chunks(plain);
  std::vector<RawChunk> b = test::split_chunks(file);
  NEED(a.size() == b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].id == kChunkCrc) {
      continue;
    }
    if (a[i].id == kChunkInst && (a[i].payload[2] == kSampleSlot || a[i].payload[2] == kModuleSlot)) {
      continue;
    }
    CHECK(a[i].payload == b[i].payload);
  }
  // 1.9 is the 1.8 layout whole: a Module instrument with a letter is as long as the same one with RATE 1 (a 1.8 chunk) and no letter
  const std::uint8_t none[6] = {0, 0, 0, 0, 0, 0};
  set_letters(proj(0), kModuleSlot, none);
  Instrument mod = *proj(0).instrument(kModuleSlot);
  mod.rate = 1;
  NEED(proj(0).set_instrument(kModuleSlot, mod) == Status::Ok);
  const Bytes rate_only = saved(proj(0));
  CHECK(inst_version(rate_only, kModuleSlot) == kInstVersionRate);
  CHECK(inst_size(rate_only, kModuleSlot) == inst_size(file, kModuleSlot));
}

TEST_CASE("param auto: letters round-trip byte for byte (save, load, save) on a Sample and a Module instrument") {
  test::fill_small(proj(0));
  const std::uint8_t sample_codes[6] = {instrument_param::Volume + 1, 0, instrument_param::Cutoff + 1, 0, 0, 120};
  const std::uint8_t module_codes[6] = {1, 0, 0, 32, 0, 0};
  set_letters(proj(0), kSampleSlot, sample_codes);
  set_letters(proj(0), kModuleSlot, module_codes);
  const Bytes file = saved(proj(0));
  Report rep;
  NEED(test::load(test::arena(1), file, &rep) == Progress::Done);
  CHECK_FALSE(rep.newer_minor);
  CHECK(rep.error == Error::None);
  for (unsigned i = 0; i < 6; ++i) {
    CHECK(proj(1).instrument(kSampleSlot)->midi_cc[i] == sample_codes[i]);
    CHECK(proj(1).instrument(kModuleSlot)->midi_cc[i] == module_codes[i]);
  }
  const Bytes back = saved(proj(1));
  CHECK(back == file);
  CHECK(formats::crc32(back.data(), back.size()) == formats::crc32(file.data(), file.size()));
  AutoMap m0;
  AutoMap m1;
  auto_scan(proj(0), m0);
  auto_scan(proj(1), m1);
  for (unsigned l = 0; l < kAutoLetters; ++l) {
    CHECK(m0.id[l] == m1.id[l]);
  }
}

TEST_CASE("param auto: an older chunk (1.3 .. 1.8) of a Sample or Module instrument reads its CC bytes as 0 and is written back as it was; a MIDI instrument keeps them") {
  fill_with_midi(proj(0));
  Instrument midi = *proj(0).instrument(kMidiSlot);
  midi.midi_cc[0] = 76;
  NEED(proj(0).set_instrument(kMidiSlot, midi) == Status::Ok);
  const std::uint8_t codes[6] = {instrument_param::Volume + 1, 2, 3, 0, 0, 6};
  set_letters(proj(0), kSampleSlot, codes);
  set_letters(proj(0), kModuleSlot, codes);
  const Bytes file = saved(proj(0));
  // the same file with the two non-MIDI chunks labelled as the version they had before (1.8 is the layout they carry): a reader of that time never saw a letter
  std::vector<RawChunk> cs = test::split_chunks(file);
  for (const std::uint32_t slot : {kSampleSlot, kModuleSlot}) {
    RawChunk* c = inst_of(cs, slot);
    NEED(c != nullptr);
    NEED(c->payload[1] == 0x09);
    c->payload[1] = 0x08;
  }
  Report rep;
  NEED(test::load(test::arena(1), test::build_file(cs, {}), &rep) == Progress::Done);
  CHECK_FALSE(rep.newer_minor);
  CHECK(rep.settings_skipped == 2u);  // a stray byte of a pre-1.9 chunk is zeroed and counted, once per chunk (the report's "values changed" line)
  for (unsigned i = 0; i < 6; ++i) {
    CHECK(proj(1).instrument(kSampleSlot)->midi_cc[i] == 0);
    CHECK(proj(1).instrument(kModuleSlot)->midi_cc[i] == 0);
  }
  CHECK(proj(1).instrument(kMidiSlot)->midi_cc[0] == 76);
  AutoMap m;
  auto_scan(proj(1), m);
  CHECK(auto_first_free(m) == 0);
  // saved again, the file has no letter: it is the file of the same project without them
  const std::uint8_t none[6] = {0, 0, 0, 0, 0, 0};
  set_letters(proj(0), kSampleSlot, none);
  set_letters(proj(0), kModuleSlot, none);
  CHECK(saved(proj(1)) == saved(proj(0)));
}

TEST_CASE("param auto: a later minor (1.10) keeps the letters, so a newer file is read by this reader as it is") {
  test::fill_small(proj(0));
  const std::uint8_t codes[6] = {0, 5, 0, 0, 0, 0};
  set_letters(proj(0), kModuleSlot, codes);
  const Bytes file = saved(proj(0));
  std::vector<RawChunk> cs = test::split_chunks(file);
  RawChunk* c = inst_of(cs, kModuleSlot);
  NEED(c != nullptr);
  c->payload[1] = 0x0A;
  c->payload.push_back(0xAB);  // a field of 1.10
  Report rep;
  NEED(test::load(test::arena(1), test::build_file(cs, {}), &rep) == Progress::Done);
  CHECK(rep.newer_minor);
  CHECK(proj(1).instrument(kModuleSlot)->midi_cc[1] == 5);
  CHECK(saved(proj(1)) == file);
}
