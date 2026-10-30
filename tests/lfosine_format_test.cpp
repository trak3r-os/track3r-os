// SPDX-License-Identifier: GPL-3.0-or-later
// OS-LFOSINE (docs/specs/OS-LFOSINE.md, docs/formats/o3-project.md): INST 1.4 adds no byte; the version only says that an LFO shape of 5 is Sine. The write condition (1.4 only for an instrument with a Sine
// LFO: a project that never used it is the file it was), the round trip, the older files (a shape of 5 in a chunk before 1.4 reads as Triangle, which it played), a shape above 5 reading as Triangle in
// any minor, the layout equal to 1.3's, and a newer minor (1.5) read whole.
#include <doctest/doctest.h>

#include "support/iff.h"
#include "support/need.h"
#include "support/project_fixture.h"

#include <o3os/formats/native.h>
#include <o3os/model/instrument.h>
#include <o3os/model/project.h>

#include <cstdint>
#include <vector>

using namespace o3os;
using namespace o3os::formats::native;
using namespace o3os::model;
using o3os::test::RawChunk;
using Bytes = std::vector<std::uint8_t>;

namespace {

Project& proj(int i = 0) { return test::arena(i).project; }

Bytes build(const std::vector<RawChunk>& cs) { return test::build_file(cs, {}); }

Bytes saved(const Project& p) {
  Bytes out;
  CHECK(test::save(p, out) == Progress::Done);
  return out;
}

RawChunk* inst_of(std::vector<RawChunk>& cs, std::uint8_t slot) {
  for (RawChunk& c : cs) {
    if (c.id == kChunkInst && c.payload.size() > 2 && c.payload[2] == slot) {
      return &c;
    }
  }
  return nullptr;
}

constexpr unsigned kSlot = 3;

// A sample instrument in kSlot with the six LFO shapes as given (speeds and amounts distinct, so that a mix-up of fields shows).
void put_shapes(Project& p, const std::uint8_t (&shapes)[6]) {
  Instrument in;
  init_instrument(in, InstrumentKind::Sample);
  for (unsigned i = 0; i < 6; ++i) {
    in.sampler.lfo[i].shape = shapes[i];
    in.sampler.lfo[i].speed = static_cast<std::uint8_t>(i);
    in.sampler.lfo[i].amount = 0.25f + 0.1f * static_cast<float>(i);
  }
  CHECK(p.set_instrument(kSlot, in) == Status::Ok);
}

// Loads `file` into arena `arena_index`; the instrument of kSlot, nullptr when the load failed.
const Instrument* loaded(const Bytes& file, int arena_index = 1, Report* rep = nullptr) {
  const bool ok = test::load(test::arena(arena_index), file, rep) == Progress::Done;
  CHECK(ok);
  return ok ? proj(arena_index).instrument(kSlot) : nullptr;
}

}  // namespace

TEST_CASE("lfosine: the model: the shape constants, a fresh instrument is Triangle and uses no Sine") {
  CHECK(kLfoShapeSine == 5);
  CHECK(kLfoShapeTriangle == 2);
  Instrument in;
  init_instrument(in, InstrumentKind::Sample);
  for (const Lfo& l : in.sampler.lfo) {
    CHECK(l.shape == kLfoShapeTriangle);
  }
  CHECK_FALSE(inst_uses_sine(in));
  in.sampler.lfo[4].shape = kLfoShapeSine;
  CHECK(inst_uses_sine(in));
  in.sampler.lfo[4].shape = 4;
  CHECK_FALSE(inst_uses_sine(in));
}

TEST_CASE("lfosine: INST 1.4 only for an instrument with a Sine LFO; the others are the chunks they were; the layout is 1.3's") {
  test::fill_small(proj(0));
  const std::uint8_t none[6] = {0, 1, 2, 3, 4, 2};
  put_shapes(proj(0), none);
  std::vector<RawChunk> cs = test::split_chunks(saved(proj(0)));
  RawChunk* c = inst_of(cs, kSlot);
  NEED(c != nullptr);
  CHECK(c->payload[1] == 0x01);  // the shapes 0..4 never made a 1.4 (a sample instrument at its defaults is the 1.1 chunk)
  const std::size_t size_11 = c->payload.size();

  const std::uint8_t one[6] = {0, 1, 2, 3, 4, 5};  // the finetune LFO is Sine
  put_shapes(proj(0), one);
  cs = test::split_chunks(saved(proj(0)));
  c = inst_of(cs, kSlot);
  NEED(c != nullptr);
  CHECK(c->payload[0] == 0x01);
  CHECK(c->payload[1] == 0x04);
  // 1.4 is the 1.3 layout whole: the engine byte and the twelve are there (13 bytes more than the 1.1 chunk of the same instrument), and nothing is appended.
  CHECK(c->payload.size() == size_11 + 13u);

  // An instrument with a VEL> edit and no Sine is 1.3, the same size.
  put_shapes(proj(0), none);
  Instrument v13 = *proj(0).instrument(kSlot);
  v13.vel_sens = 7;
  NEED(proj(0).set_instrument(kSlot, v13) == Status::Ok);
  cs = test::split_chunks(saved(proj(0)));
  c = inst_of(cs, kSlot);
  NEED(c != nullptr);
  CHECK(c->payload[1] == 0x03);
  CHECK(c->payload.size() == size_11 + 13u);
}

TEST_CASE("lfosine: a file with Sine on every slot round-trips (save, load, save) and every other field is kept") {
  test::fill_small(proj(0));
  const std::uint8_t all[6] = {5, 5, 5, 5, 5, 5};
  put_shapes(proj(0), all);
  const Bytes file = saved(proj(0));
  Report rep;
  const Instrument* q = loaded(file, 1, &rep);
  NEED(q != nullptr);
  CHECK_FALSE(rep.newer_minor);
  for (unsigned i = 0; i < 6; ++i) {
    CHECK(q->sampler.lfo[i].shape == kLfoShapeSine);
    CHECK(q->sampler.lfo[i].speed == i);
    CHECK(q->sampler.lfo[i].amount == doctest::Approx(0.25f + 0.1f * static_cast<float>(i)));
  }
  CHECK(saved(proj(1)) == file);

  const std::uint8_t mixed[6] = {2, 5, 0, 5, 4, 1};
  put_shapes(proj(0), mixed);
  const Bytes file2 = saved(proj(0));
  q = loaded(file2);
  NEED(q != nullptr);
  for (unsigned i = 0; i < 6; ++i) {
    CHECK(q->sampler.lfo[i].shape == mixed[i]);
  }
  CHECK(saved(proj(1)) == file2);
}

TEST_CASE("lfosine: an old file reads a shape of 5 as Triangle (it played as one); a shape above 5 is Triangle in every minor; 1.4 keeps 5") {
  test::fill_small(proj(0));
  const std::uint8_t sine[6] = {5, 5, 5, 5, 5, 5};
  put_shapes(proj(0), sine);
  const Bytes file = saved(proj(0));
  for (const std::uint8_t minor : {std::uint8_t{1}, std::uint8_t{2}, std::uint8_t{3}}) {  // the same fields under an older label: the file an older build wrote with a stray 5
    std::vector<RawChunk> cs = test::split_chunks(file);
    RawChunk* c = inst_of(cs, kSlot);
    NEED(c != nullptr);
    if (minor == 0x01) {
      c->payload.resize(c->payload.size() - 13);  // no engine byte, no twelve
    } else if (minor == 0x02) {
      c->payload.resize(c->payload.size() - 12);  // the engine byte, no twelve
    }
    c->payload[1] = minor;
    const Instrument* q = loaded(build(cs));
    NEED(q != nullptr);
    for (const Lfo& l : q->sampler.lfo) {
      CHECK_MESSAGE(l.shape == kLfoShapeTriangle, "minor " << static_cast<int>(minor));
    }
    // ... and written back as the file an older build would have made (no Sine, so no 1.4)
    std::vector<RawChunk> back = test::split_chunks(saved(proj(1)));
    RawChunk* b = inst_of(back, kSlot);
    NEED(b != nullptr);
    CHECK(b->payload[1] <= 0x03);
  }
  // A 1.4 chunk keeps 5 and holds a larger shape to Triangle (a shape of a later version).
  const std::uint8_t big[6] = {5, 6, 9, 255, 5, 0};
  put_shapes(proj(0), big);
  std::vector<RawChunk> cs = test::split_chunks(saved(proj(0)));
  RawChunk* c = inst_of(cs, kSlot);
  NEED(c != nullptr);
  CHECK(c->payload[1] == 0x04);
  const Instrument* q = loaded(build(cs));
  NEED(q != nullptr);
  const std::uint8_t want[6] = {5, 2, 2, 2, 5, 0};
  for (unsigned i = 0; i < 6; ++i) {
    CHECK(q->sampler.lfo[i].shape == want[i]);
  }
  // A 1.1 chunk with a shape above 5: Triangle too (the file an older build could have written).
  const std::uint8_t stray[6] = {7, 2, 2, 2, 2, 2};
  put_shapes(proj(0), stray);
  cs = test::split_chunks(saved(proj(0)));
  c = inst_of(cs, kSlot);
  NEED(c != nullptr);
  q = loaded(build(cs));
  NEED(q != nullptr);
  CHECK(q->sampler.lfo[0].shape == kLfoShapeTriangle);
}

TEST_CASE("lfosine: a newer minor (1.6; 1.5 is OS-STEREO's) is read whole, the Sine kept, and written back as 1.4 without the appended bytes") {
  test::fill_small(proj(0));
  const std::uint8_t sine[6] = {2, 2, 5, 2, 2, 2};
  put_shapes(proj(0), sine);
  const Bytes file = saved(proj(0));
  std::vector<RawChunk> cs = test::split_chunks(file);
  RawChunk* c = inst_of(cs, kSlot);
  NEED(c != nullptr);
  c->payload[1] = 0x06;
  for (unsigned i = 0; i < 40; ++i) {
    c->payload.push_back(0xCD);  // a field of a later version
  }
  Report rep;
  const Instrument* q = loaded(build(cs), 1, &rep);
  NEED(q != nullptr);
  CHECK(rep.newer_minor);
  CHECK(q->sampler.lfo[2].shape == kLfoShapeSine);
  CHECK(saved(proj(1)) == file);
}
