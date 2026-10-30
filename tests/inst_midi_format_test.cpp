// SPDX-License-Identifier: GPL-3.0-or-later
// OS-INSTUI S8b + S9, INST 1.3 (docs/formats/o3-project.md, docs/formats/INST-1.3-diff.md): the twelve bytes a MIDI instrument's fields and a sample instrument's VEL> add after the engine byte.
// The exact bytes (written here from the description, not by the writer), the write condition (only when one of the twelve is not its default: a project that never used them is the file it was),
// the round trip, the older files (1.1, 1.2) read as the defaults, a value out of its range reading as the field's default, the strict length, a newer minor (1.5) read as far as 1.3, and the
// rule that an older reader keeps the instrument and loses the twelve bytes (checked here by the same minor-version rule the 1.2 reader applies to a 1.3 chunk: the engine byte is read, the rest skipped).
#include <doctest/doctest.h>

#include "support/iff.h"
#include "support/need.h"
#include "support/project_fixture.h"

#include <o3os/formats/crc32.h>
#include <o3os/formats/native.h>
#include <o3os/model/instrument.h>
#include <o3os/model/project.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace o3os;
using namespace o3os::formats::native;
using namespace o3os::model;
using o3os::test::RawChunk;
using Bytes = std::vector<std::uint8_t>;

namespace {

Project& proj(int i = 0) { return test::arena(i).project; }

Bytes build(const std::vector<RawChunk>& cs) { return test::build_file(cs, {}); }

// The payload of the INST chunk of slot `slot` in `cs`.
RawChunk* inst_of(std::vector<RawChunk>& cs, std::uint8_t slot) {
  for (RawChunk& c : cs) {
    if (c.id == kChunkInst && c.payload.size() > 2 && c.payload[2] == slot) {
      return &c;
    }
  }
  return nullptr;
}

// A MIDI instrument with all twelve fields away from their defaults.
Instrument busy_midi() {
  Instrument in;
  init_instrument(in, InstrumentKind::Midi);
  in.midi_channel = 5;
  in.midi_velocity = 100;
  in.midi_program = 6;  // program 5
  in.midi_out = kMidiOutBoth;
  in.midi_pb_range = 12;
  in.midi_offset = 40;
  const std::uint8_t cc[6] = {75, kMidiCcProg, kMidiCcChanAt, kMidiCcPolyAt, 1, kMidiCcNumberMax};
  for (unsigned i = 0; i < 6; ++i) {
    in.midi_cc[i] = cc[i];
  }
  in.vel_sens = 33;
  return in;
}

// The twelve bytes of the description, in file order.
const std::uint8_t kBusyBytes[12] = {100, 6, 3, 12, 40, 75, 129, 130, 131, 1, 128, 33};

std::uint32_t file_crc(const Bytes& b) { return formats::crc32(b.data(), b.size()); }

constexpr unsigned kSlot = 20;

Bytes saved(const Project& p) {
  Bytes out;
  CHECK(test::save(p, out) == Progress::Done);
  return out;
}

}  // namespace

TEST_CASE("inst 1.3: the model: every field has its default in a fresh instrument, and validate refuses a value past its range") {
  for (const InstrumentKind kind : {InstrumentKind::Sample, InstrumentKind::Midi, InstrumentKind::Module}) {
    Instrument in;
    init_instrument(in, kind);
    CHECK(in.midi_velocity == 127);
    CHECK(in.midi_program == kMidiProgramNone);
    CHECK(in.midi_out == kMidiOutConfig);
    CHECK(in.midi_pb_range == 2);
    CHECK(in.midi_offset == 0);
    for (const std::uint8_t cc : in.midi_cc) {
      CHECK(cc == kMidiCcConfig);
    }
    CHECK(in.vel_sens == 100);
    CHECK(inst_v13_default(in));
  }
  Instrument in = busy_midi();
  CHECK_FALSE(inst_v13_default(in));
  CHECK(validate_instrument(in, 0) == nullptr);
  const auto refused = [&](auto edit) {
    Instrument bad = busy_midi();
    edit(bad);
    CHECK(validate_instrument(bad, 0) != nullptr);
  };
  refused([](Instrument& i) { i.midi_velocity = 0; });
  refused([](Instrument& i) { i.midi_velocity = 128; });
  refused([](Instrument& i) { i.midi_program = 129; });
  refused([](Instrument& i) { i.midi_out = 4; });
  refused([](Instrument& i) { i.midi_pb_range = 25; });
  refused([](Instrument& i) { i.midi_offset = 101; });
  refused([](Instrument& i) { i.midi_cc[3] = 132; });
  refused([](Instrument& i) { i.vel_sens = 101; });
  CHECK(sizeof(Instrument) == 720);
}

TEST_CASE("inst 1.3: the chunk is the bytes of the description: version 1.3, the engine byte, then the twelve") {
  test::fill_small(proj(0));
  NEED(proj(0).set_instrument(kSlot, busy_midi()) == Status::Ok);
  const Bytes file = saved(proj(0));
  std::vector<RawChunk> cs = test::split_chunks(file);
  RawChunk* c = inst_of(cs, kSlot);
  NEED(c != nullptr);
  CHECK(c->payload[0] == 0x01);
  CHECK(c->payload[1] == 0x03);
  // The same instrument with the twelve at their defaults is the 1.1 chunk it always was; the 1.3 chunk is that plus the engine byte and the twelve.
  Instrument dflt = busy_midi();
  dflt.midi_velocity = kMidiVelocityDefault;
  dflt.midi_program = 0;
  dflt.midi_out = 0;
  dflt.midi_pb_range = 2;
  dflt.midi_offset = 0;
  for (std::uint8_t& cc : dflt.midi_cc) {
    cc = 0;
  }
  dflt.vel_sens = 100;
  NEED(proj(0).set_instrument(kSlot, dflt) == Status::Ok);
  std::vector<RawChunk> cd = test::split_chunks(saved(proj(0)));
  RawChunk* d = inst_of(cd, kSlot);
  NEED(d != nullptr);
  CHECK(d->payload[1] == 0x01);
  NEED(c->payload.size() == d->payload.size() + 13u);
  Bytes tail(c->payload.end() - 13, c->payload.end());
  CHECK(tail[0] == 0);  // the engine: automatic
  for (unsigned i = 0; i < 12; ++i) {
    CHECK(tail[1 + i] == kBusyBytes[i]);
  }
  // everything before the engine byte is the 1.1 chunk's, but for the version
  Bytes head(c->payload.begin() + 2, c->payload.end() - 13);
  Bytes head_old(d->payload.begin() + 2, d->payload.end());
  CHECK(head == head_old);
}

TEST_CASE("inst 1.3: written only when one of the twelve is not its default; 1.2 for an engine alone; the default project is the file it was") {
  test::fill_small(proj(0));
  const Bytes before = saved(proj(0));
  for (const RawChunk& c : test::split_chunks(before)) {
    if (c.id == kChunkInst) {
      CHECK(c.payload[1] == 0x01);  // no engine, no field: INST 1.1
    }
  }
  // each of the twelve alone makes its instrument a 1.3 chunk, and nothing else changes version
  for (unsigned f = 0; f < 12; ++f) {
    test::fill_small(proj(0));
    Instrument in;
    init_instrument(in, f == 11 ? InstrumentKind::Sample : InstrumentKind::Midi);
    if (f != 11) {
      in.midi_channel = 2;
    }
    switch (f) {
      case 0: in.midi_velocity = 126; break;
      case 1: in.midi_program = 1; break;
      case 2: in.midi_out = kMidiOutUsb; break;
      case 3: in.midi_pb_range = 0; break;
      case 4: in.midi_offset = 1; break;
      case 11: in.vel_sens = 0; break;
      default: in.midi_cc[f - 5] = kMidiCcProg; break;
    }
    NEED(proj(0).set_instrument(kSlot, in) == Status::Ok);
    unsigned v13 = 0;
    for (const RawChunk& c : test::split_chunks(saved(proj(0)))) {
      if (c.id == kChunkInst && c.payload[1] == 0x03) {
        ++v13;
        CHECK(c.payload[2] == kSlot);
      }
    }
    CHECK(v13 == 1u);
  }
  // an engine alone is the 1.2 chunk of OS-004d, byte for byte what that writer made: the engine byte at the end
  test::fill_small(proj(0));
  Instrument s = *proj(0).instrument(0);
  s.engine = kInstEngineAmiga;
  NEED(proj(0).set_instrument(0, s) == Status::Ok);
  std::vector<RawChunk> cs = test::split_chunks(saved(proj(0)));
  RawChunk* c = inst_of(cs, 0);
  NEED(c != nullptr);
  CHECK(c->payload[1] == 0x02);
  CHECK(c->payload.back() == kInstEngineAmiga);
  // an engine together with a field: 1.3 carries the engine in its own byte
  s.vel_sens = 90;
  NEED(proj(0).set_instrument(0, s) == Status::Ok);
  cs = test::split_chunks(saved(proj(0)));
  c = inst_of(cs, 0);
  NEED(c != nullptr);
  CHECK(c->payload[1] == 0x03);
  CHECK(c->payload[c->payload.size() - 13] == kInstEngineAmiga);
  CHECK(c->payload.back() == 90);
}

TEST_CASE("inst 1.3: a file that has the fields round-trips (save, load, save), for every kind") {
  test::fill_small(proj(0));
  Instrument s = *proj(0).instrument(0);  // a sample instrument with VEL>
  s.vel_sens = 7;
  NEED(proj(0).set_instrument(0, s) == Status::Ok);
  NEED(proj(0).set_instrument(kSlot, busy_midi()) == Status::Ok);
  const Bytes file = saved(proj(0));
  Report rep;
  NEED(test::load(test::arena(1), file, &rep) == Progress::Done);
  CHECK_FALSE(rep.newer_minor);
  const Instrument* q = proj(1).instrument(kSlot);
  NEED(q != nullptr);
  CHECK(q->midi_channel == 5);
  CHECK(q->midi_velocity == 100);
  CHECK(q->midi_program == 6);
  CHECK(q->midi_out == kMidiOutBoth);
  CHECK(q->midi_pb_range == 12);
  CHECK(q->midi_offset == 40);
  for (unsigned i = 0; i < 6; ++i) {
    CHECK(q->midi_cc[i] == kBusyBytes[5 + i]);
  }
  CHECK(q->vel_sens == 33);
  CHECK(proj(1).instrument(0)->vel_sens == 7);
  CHECK(proj(1).instrument(0)->midi_velocity == 127);  // the fields of the other kind stay at their defaults
  Bytes back = saved(proj(1));
  CHECK(back == file);
  CHECK(file_crc(back) == file_crc(file));
}

TEST_CASE("inst 1.3: an older file (1.1, 1.2) reads as the defaults, and is written back as it was") {
  test::fill_small(proj(0));
  NEED(proj(0).set_instrument(kSlot, busy_midi()) == Status::Ok);
  std::vector<RawChunk> cs = test::split_chunks(saved(proj(0)));
  RawChunk* c = inst_of(cs, kSlot);
  NEED(c != nullptr);
  // 1.2: take the twelve off and relabel
  RawChunk keep = *c;
  c->payload[1] = 0x02;
  c->payload.resize(c->payload.size() - 12);
  Report rep;
  NEED(test::load(test::arena(1), build(cs), &rep) == Progress::Done);
  CHECK_FALSE(rep.newer_minor);
  const Instrument* q = proj(1).instrument(kSlot);
  NEED(q != nullptr);
  CHECK(inst_v13_default(*q));
  CHECK(q->midi_channel == 5);
  CHECK(q->engine == 0);
  // 1.1: the engine byte too
  c->payload[1] = 0x01;
  c->payload.pop_back();
  NEED(test::load(test::arena(1), build(cs), &rep) == Progress::Done);
  q = proj(1).instrument(kSlot);
  NEED(q != nullptr);
  CHECK(inst_v13_default(*q));
  Bytes resaved = saved(proj(1));
  for (const RawChunk& r : test::split_chunks(resaved)) {
    if (r.id == kChunkInst) {
      CHECK(r.payload[1] == 0x01);  // written back as 1.1: the file an older build wrote
    }
  }
  CHECK(resaved == build(cs));
  (void)keep;
  // an older sample instrument keeps its volume law: VEL> reads 100
  CHECK(proj(1).instrument(0)->vel_sens == 100);
}

TEST_CASE("inst 1.3: a value out of its range reads as the field's default (a later version may add values), and the chunk is exact") {
  test::fill_small(proj(0));
  NEED(proj(0).set_instrument(kSlot, busy_midi()) == Status::Ok);
  const std::vector<RawChunk> cs = test::split_chunks(saved(proj(0)));
  // byte i of the twelve (file order) -> a value past its range -> the field's default
  struct Bad {
    unsigned at;
    std::uint8_t value;
    std::uint8_t dflt;
  };
  const Bad bad[] = {{0, 0, 127}, {0, 200, 127}, {1, 129, 0}, {2, 4, 0}, {3, 25, 2}, {4, 101, 0}, {5, 132, 0}, {10, 255, 0}, {11, 101, 100}};
  for (const Bad& b : bad) {
    std::vector<RawChunk> edit = cs;
    RawChunk* c = inst_of(edit, kSlot);
    NEED(c != nullptr);
    c->payload[c->payload.size() - 12 + b.at] = b.value;
    Report rep;
    NEED(test::load(test::arena(1), build(edit), &rep) == Progress::Done);
    const Instrument* q = proj(1).instrument(kSlot);
    NEED(q != nullptr);
    const std::uint8_t got[12] = {q->midi_velocity, q->midi_program, q->midi_out, q->midi_pb_range, q->midi_offset, q->midi_cc[0], q->midi_cc[1], q->midi_cc[2], q->midi_cc[3], q->midi_cc[4], q->midi_cc[5], q->vel_sens};
    CHECK(got[b.at] == b.dflt);
    for (unsigned i = 0; i < 12; ++i) {
      if (i != b.at) {
        CHECK(got[i] == kBusyBytes[i]);  // the others are as written
      }
    }
  }
  // 1.3 a byte short or a byte long: Malformed (unused or missing bytes of this minor)
  std::vector<RawChunk> edit = cs;
  inst_of(edit, kSlot)->payload.pop_back();
  Report rep;
  CHECK(test::load(test::arena(1), build(edit), &rep) == Progress::Failed);
  CHECK(rep.error == Error::Malformed);
  edit = cs;
  inst_of(edit, kSlot)->payload.push_back(0);
  CHECK(test::load(test::arena(1), build(edit), &rep) == Progress::Failed);
  CHECK(rep.error == Error::Malformed);
  // 1.2 with the twelve left on (a chunk that says 1.2 and has 12 more bytes): Malformed too
  edit = cs;
  inst_of(edit, kSlot)->payload[1] = 0x02;
  CHECK(test::load(test::arena(1), build(edit), &rep) == Progress::Failed);
  CHECK(rep.error == Error::Malformed);
}

TEST_CASE("inst 1.3: a newer minor (1.6, past INST 1.4 of OS-LFOSINE and INST 1.5 of OS-STEREO) is read as far as 1.5, the rest skipped, and written back as 1.3 without it; an older reader meets the same rule at 1.3") {
  test::fill_small(proj(0));
  NEED(proj(0).set_instrument(kSlot, busy_midi()) == Status::Ok);
  const Bytes file = saved(proj(0));
  std::vector<RawChunk> edit = test::split_chunks(file);
  RawChunk* c = inst_of(edit, kSlot);
  NEED(c != nullptr);
  c->payload[1] = 0x06;
  for (unsigned i = 0; i < 300; ++i) {
    c->payload.push_back(0xAB);  // the first is a stereo flag above 1 (mono); the rest are fields of a later version
  }
  Report rep;
  NEED(test::load(test::arena(1), build(edit), &rep) == Progress::Done);
  CHECK(rep.newer_minor);
  const Instrument* q = proj(1).instrument(kSlot);
  NEED(q != nullptr);
  CHECK(q->midi_velocity == 100);
  CHECK(q->midi_cc[5] == kMidiCcNumberMax);
  CHECK(q->vel_sens == 33);
  CHECK(saved(proj(1)) == file);  // the appended bytes are lost on the next save; the twelve are kept
  // The rule an older reader (INST 1.2) applies to a 1.3 chunk is this one a minor lower: it reads the instrument as of 1.2 (the rate, the engine) and skips the twelve. The instrument stays whole:
  // the same chunk with the twelve cut off and labelled 1.2 is exactly what that reader makes of it, and it loads with every older field intact.
  edit = test::split_chunks(file);
  c = inst_of(edit, kSlot);
  NEED(c != nullptr);
  c->payload[1] = 0x02;
  c->payload.resize(c->payload.size() - 12);
  NEED(test::load(test::arena(1), build(edit), &rep) == Progress::Done);
  q = proj(1).instrument(kSlot);
  NEED(q != nullptr);
  CHECK(q->kind == InstrumentKind::Midi);
  CHECK(q->midi_channel == 5);
  CHECK(std::string(q->name) == std::string(proj(0).instrument(kSlot)->name));
}

TEST_CASE("inst 1.3: the maximal project (64 instruments, every limit) still fits the file limit with the twelve bytes on all of them") {
  test::fill_maximal(proj(0));
  for (unsigned i = 0; i < kMaxInstruments; ++i) {
    Instrument in = *proj(0).instrument(i);
    in.midi_offset = 9;
    in.vel_sens = 55;
    in.midi_velocity = 64;
    NEED(proj(0).set_instrument(i, in) == Status::Ok);
  }
  const Bytes file = saved(proj(0));
  CHECK(file.size() < kMaxFileBytes);
  Report rep;
  NEED(test::load(test::arena(1), file, &rep) == Progress::Done);
  CHECK(proj(1).instrument(63)->midi_offset == 9);
  CHECK(saved(proj(1)) == file);
}

TEST_CASE("inst 1.3: the fixtures that never used the fields are the files they were: size and CRC-32 of the build before INST 1.3") {
  const struct {
    void (*fill)(Project&);
    std::size_t size;
    std::uint32_t crc;
  } pins[] = {{test::fill_small, 2648, 0x3f04f4e7u}, {test::fill_state, 3372, 0xa8d283e1u}, {test::fill_persist2, 3032, 0x40ad3678u}, {test::fill_maximal, 1888426, 0xc58be9e8u}};  // (the maximal's CRC: its LFO shapes are 0..4 since OS-LFOSINE, same size, shape bytes only)
  for (const auto& p : pins) {
    p.fill(proj(0));
    const Bytes file = saved(proj(0));
    CHECK(file.size() == p.size);
    CHECK(file_crc(file) == p.crc);
  }
}

// ---- OS-STEREO step 2, INST 1.5: the instrument's stereo flag (docs/formats/o3-project.md; NEW COMMUNITY OS BEHAVIOUR). The pins above also hold the mono projects of this build to the files they were. ----

TEST_CASE("inst 1.5: the model: a fresh instrument is mono, the flag is a Sample instrument's, and validate refuses a value above 1 and the flag on another kind") {
  for (const InstrumentKind kind : {InstrumentKind::Sample, InstrumentKind::Midi, InstrumentKind::Module}) {
    Instrument in;
    init_instrument(in, kind);
    CHECK(in.stereo == 0);
  }
  Instrument s;
  init_instrument(s, InstrumentKind::Sample);
  s.stereo = 1;
  CHECK(validate_instrument(s, 0) == nullptr);
  s.stereo = 2;
  CHECK(validate_instrument(s, 0) != nullptr);
  Instrument m = busy_midi();
  m.stereo = 1;
  CHECK(validate_instrument(m, 0) != nullptr);
  CHECK(proj(0).set_instrument(kSlot, m) != Status::Ok);
  Instrument mod;
  init_instrument(mod, InstrumentKind::Module);
  mod.stereo = 1;
  CHECK(validate_instrument(mod, 1) != nullptr);
}

TEST_CASE("inst 1.5: a stereo sample instrument is the 1.5 chunk (the 1.3 layout and the flag), and the project round-trips (save, load, save)") {
  test::fill_small(proj(0));
  const Bytes mono = saved(proj(0));
  Instrument s = *proj(0).instrument(0);
  s.stereo = 1;
  NEED(proj(0).set_instrument(0, s) == Status::Ok);
  const Bytes file = saved(proj(0));
  std::vector<RawChunk> cs = test::split_chunks(file);
  RawChunk* c = inst_of(cs, 0);
  NEED(c != nullptr);
  CHECK(c->payload[0] == 0x01);
  CHECK(c->payload[1] == 0x05);
  CHECK(c->payload.back() == 1);  // the flag is the last byte
  // before it: the 1.3 block, whole (the engine byte, the twelve at their defaults), as an instrument of 1.3 has it
  std::vector<RawChunk> ms = test::split_chunks(mono);
  RawChunk* m = inst_of(ms, 0);
  NEED(m != nullptr);
  NEED(c->payload.size() == m->payload.size() + 13u + 1u);  // 1.1 -> the engine byte, the twelve, the flag
  const std::uint8_t dflt[13] = {0, 127, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 100};
  for (unsigned i = 0; i < 13; ++i) {
    CHECK(c->payload[c->payload.size() - 14 + i] == dflt[i]);
  }
  // the other instruments are the chunks of the mono file
  for (std::uint8_t slot = 1; slot < 3; ++slot) {
    RawChunk* a = inst_of(cs, slot);
    RawChunk* b = inst_of(ms, slot);
    if (a != nullptr && b != nullptr) {
      CHECK(a->payload == b->payload);
    }
  }
  Report rep;
  NEED(test::load(test::arena(1), file, &rep) == Progress::Done);
  CHECK_FALSE(rep.newer_minor);
  CHECK(proj(1).instrument(0)->stereo == 1);
  CHECK(proj(1).instrument(0)->kind == InstrumentKind::Sample);
  const Bytes back = saved(proj(1));
  CHECK(back == file);
  CHECK(file_crc(back) == file_crc(file));
  // flag off again: the mono file's bytes, byte for byte
  s.stereo = 0;
  NEED(proj(0).set_instrument(0, s) == Status::Ok);
  CHECK(saved(proj(0)) == mono);
}

TEST_CASE("inst 1.5: the flag with an engine, VEL> or a Sine LFO is one 1.5 chunk that keeps all of them") {
  test::fill_small(proj(0));
  Instrument s = *proj(0).instrument(0);
  s.stereo = 1;
  s.engine = kInstEngineAmiga;
  s.vel_sens = 40;
  s.sampler.lfo[1].shape = kLfoShapeSine;
  NEED(proj(0).set_instrument(0, s) == Status::Ok);
  const Bytes file = saved(proj(0));
  Report rep;
  NEED(test::load(test::arena(1), file, &rep) == Progress::Done);
  const Instrument* q = proj(1).instrument(0);
  NEED(q != nullptr);
  CHECK(q->stereo == 1);
  CHECK(q->engine == kInstEngineAmiga);
  CHECK(q->vel_sens == 40);
  CHECK(q->sampler.lfo[1].shape == kLfoShapeSine);  // (the Sine gate is minor >= 4: 1.5 is above it)
  CHECK(saved(proj(1)) == file);
}

TEST_CASE("inst 1.5: a 1.4 chunk reads mono, a value above 1 reads mono, and neither is written back as 1.5") {
  test::fill_small(proj(0));
  Instrument s = *proj(0).instrument(0);
  s.stereo = 1;
  s.sampler.lfo[0].shape = kLfoShapeSine;  // so that the chunk has something that 1.4 says
  NEED(proj(0).set_instrument(0, s) == Status::Ok);
  const Bytes file = saved(proj(0));
  // 1.4: the flag taken off and the chunk labelled 1.4 (what the build before this one wrote for the instrument)
  std::vector<RawChunk> cs = test::split_chunks(file);
  RawChunk* c = inst_of(cs, 0);
  NEED(c != nullptr);
  c->payload[1] = 0x04;
  c->payload.pop_back();
  Report rep;
  NEED(test::load(test::arena(1), build(cs), &rep) == Progress::Done);
  CHECK_FALSE(rep.newer_minor);
  CHECK(proj(1).instrument(0)->stereo == 0);
  CHECK(proj(1).instrument(0)->sampler.lfo[0].shape == kLfoShapeSine);
  CHECK(saved(proj(1)) == build(cs));  // written back as 1.4
  // 1.5 with the flag at 2 (and at 255): reads 0, as an engine above the table does
  for (const std::uint8_t value : {std::uint8_t{2}, std::uint8_t{255}}) {
    cs = test::split_chunks(file);
    c = inst_of(cs, 0);
    NEED(c != nullptr);
    c->payload.back() = value;
    NEED(test::load(test::arena(1), build(cs), &rep) == Progress::Done);
    CHECK(proj(1).instrument(0)->stereo == 0);
    for (const RawChunk& r : test::split_chunks(saved(proj(1)))) {
      if (r.id == kChunkInst && r.payload[2] == 0) {
        CHECK(r.payload[1] == 0x04);  // (the Sine LFO is still there; the flag is not)
      }
    }
  }
}

TEST_CASE("inst 1.5: the length is exact, and the flag on a MIDI instrument is read as 0 (the project still loads)") {
  test::fill_small(proj(0));
  Instrument s = *proj(0).instrument(0);
  s.stereo = 1;
  NEED(proj(0).set_instrument(0, s) == Status::Ok);
  const Bytes file = saved(proj(0));
  std::vector<RawChunk> cs = test::split_chunks(file);
  Report rep;
  inst_of(cs, 0)->payload.pop_back();  // a byte short
  CHECK(test::load(test::arena(1), build(cs), &rep) == Progress::Failed);
  CHECK(rep.error == Error::Malformed);
  cs = test::split_chunks(file);
  inst_of(cs, 0)->payload.push_back(0);  // a byte long
  CHECK(test::load(test::arena(1), build(cs), &rep) == Progress::Failed);
  CHECK(rep.error == Error::Malformed);
  // a MIDI instrument of 1.3, relabelled 1.5 with the flag
  test::fill_small(proj(0));
  NEED(proj(0).set_instrument(kSlot, busy_midi()) == Status::Ok);
  cs = test::split_chunks(saved(proj(0)));
  RawChunk* m = inst_of(cs, kSlot);
  NEED(m != nullptr);
  m->payload[1] = 0x05;
  m->payload.push_back(1);
  NEED(test::load(test::arena(1), build(cs), &rep) == Progress::Done);  // the flag is a Sample instrument's: on a MIDI instrument it reads 0 (docs/specs/OS-STEREO.md), the file is not refused
  CHECK(proj(1).instrument(kSlot)->stereo == 0);
  CHECK(proj(1).instrument(kSlot)->midi_channel == 5);
  CHECK(std::memcmp(proj(1).instrument(kSlot)->reserved, "\0\0\0", 3) == 0);
  m->payload.back() = 0;  // the same chunk with the flag at 0 is a valid 1.5 chunk of a MIDI instrument (read, written back as 1.3)
  NEED(test::load(test::arena(1), build(cs), &rep) == Progress::Done);
  CHECK(proj(1).instrument(kSlot)->stereo == 0);
  CHECK(proj(1).instrument(kSlot)->midi_channel == 5);
}

TEST_CASE("inst 1.5: the maximal project with every sample instrument stereo fits the file limit and round-trips") {
  test::fill_maximal(proj(0));
  for (unsigned i = 0; i < kMaxInstruments; ++i) {
    Instrument in = *proj(0).instrument(i);
    if (in.kind == InstrumentKind::Sample) {
      in.stereo = 1;
      NEED(proj(0).set_instrument(i, in) == Status::Ok);
    }
  }
  const Bytes file = saved(proj(0));
  CHECK(file.size() < kMaxFileBytes);
  Report rep;
  NEED(test::load(test::arena(1), file, &rep) == Progress::Done);
  CHECK(saved(proj(1)) == file);
}
