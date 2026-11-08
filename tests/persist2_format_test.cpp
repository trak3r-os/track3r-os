// SPDX-License-Identifier: GPL-3.0-or-later
// OS-PERSIST2: the session state that used to be runtime only (model/project_state.h): the note-entry settings and JUMP, the tracks' MUTE MODEs, the Perform scenes with LATCH and the send
// effects' controls, as the native format's chunks NOTE, MUTE, SCEN and SNFX (docs/formats/o3-project.md section 3, "The session state"). This file is the model and the format: the rules of the
// model, the exact bytes (written here from the description, not by the writer), the round trip of every field, the file that an older build wrote (read with the defaults, and the build before
// OS-PERSIST2 writing the same bytes for the projects that have none of the four), the tolerant reader (a value out of range is its default, counted), the shape errors, the order, the unknown
// chunks that are kept and the unknown pool of an older build that keeps these. The runtime side (the App copying the board, the bank and the entry settings) is persist2_app_test.cpp.
#include <doctest/doctest.h>

#include "support/iff.h"
#include "support/memory_io.h"
#include "support/need.h"
#include "support/project_fixture.h"
#include "support/rng.h"

#include <o3os/formats/builtin.h>
#include <o3os/formats/crc32.h>
#include <o3os/formats/format.h>
#include <o3os/formats/native.h>
#include <o3os/model/editor.h>
#include <o3os/model/project.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

using namespace o3os;
using namespace o3os::formats::native;
using namespace o3os::model;
using o3os::test::RawChunk;
using Bytes = std::vector<std::uint8_t>;

namespace {

Project& proj(int i = 0) { return test::arena(i).project; }

const RawChunk* find(const std::vector<RawChunk>& cs, std::uint32_t id) {
  for (const RawChunk& c : cs) {
    if (c.id == id) {
      return &c;
    }
  }
  return nullptr;
}

bool is_session(std::uint32_t id) { return id == kChunkNote || id == kChunkMute || id == kChunkScen || id == kChunkSnfx; }

Bytes build(const std::vector<RawChunk>& cs) { return test::build_file(cs, {}); }

Bytes session_file() {
  test::fill_persist2(proj(2));
  Bytes out;
  CHECK(test::save(proj(2), out) == Progress::Done);
  return out;
}

void put16(Bytes& b, int v) {
  b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
  b.push_back(static_cast<std::uint8_t>(v & 0xFF));
}

// The payloads of the persist2 fixture, from the description of docs/formats/o3-project.md.
Bytes expected_note() { return {0x01, 0x00, 7, 9, 5, 1, 4}; }

Bytes expected_mute() {
  Bytes b = {0x01, 0x00, 16};
  for (int t = 0; t < 16; ++t) {
    b.push_back(t == 0 ? 1 : (t == 3 ? 2 : (t == 15 ? 1 : 0)));
  }
  return b;
}

// One scene: 12 slots of fx, row, three i16. `fx` is the slot's FX (the first twelve of the FX list, in order, unless a test says), `row` and `value` are what differs from nothing.
Bytes scene_bytes(int stored, const int (&fx)[12], const int (&row)[12], const int (&value)[12][3]) {
  Bytes b = {static_cast<std::uint8_t>(stored)};
  for (int s = 0; s < 12; ++s) {
    b.push_back(static_cast<std::uint8_t>(fx[s]));
    b.push_back(static_cast<std::uint8_t>(row[s]));
    for (int r = 0; r < 3; ++r) {
      put16(b, value[s][r]);
    }
  }
  return b;
}

Bytes expected_scen() {
  Bytes b = {0x01, 0x00, 0, 3};  // LATCH off, three scenes
  int fx[12], row[12], value[12][3];
  const auto reset = [&] {
    for (int s = 0; s < 12; ++s) {
      fx[s] = s + 1;
      row[s] = 0;
      value[s][0] = value[s][1] = value[s][2] = 0;
    }
  };
  reset();
  fx[2] = 0;
  fx[11] = 19;
  row[0] = 2;
  row[4] = 3;
  value[0][0] = -100;
  value[0][1] = 100;
  value[0][2] = 1;
  value[4][2] = -48;
  value[11][1] = 8;
  const Bytes s0 = scene_bytes(1, fx, row, value);
  b.insert(b.end(), s0.begin(), s0.end());
  b.insert(b.end(), 97, 0);  // scene 1 is not stored: 96 zero bytes after its flag
  reset();
  row[11] = 1;
  value[11][0] = 15;
  const Bytes s2 = scene_bytes(1, fx, row, value);
  b.insert(b.end(), s2.begin(), s2.end());
  return b;
}

Bytes expected_snfx() {
  Bytes b = {0x01, 0x00, 12};
  for (int v : {1, 1, 18, 3500, 70, 0, 1, 100, 0, 77, 15, 1}) {
    put16(b, v);
  }
  return b;
}

// The default values of the four parts, written out (not by init_*), so that a changed default fails here.
void check_defaults(const Project& p) {
  const NoteState& n = p.note();
  CHECK(n.root == 0);
  CHECK(n.scale == 0);
  CHECK(n.octave == 3);
  CHECK(n.lock == 0);
  CHECK(n.jump == 1);
  for (int t = 0; t < 16; ++t) {
    CHECK(p.mute().mode[t] == 0);
  }
  CHECK(p.scenes().latch == 1);
  for (const SceneState& sc : p.scenes().scene) {
    CHECK(sc.stored == 0);
    CHECK(sc.reserved == 0);
    const std::uint8_t* b = reinterpret_cast<const std::uint8_t*>(&sc.slots);
    bool zero = true;
    for (std::size_t i = 0; i < sizeof sc.slots; ++i) {
      zero = zero && b[i] == 0;
    }
    CHECK(zero);
  }
  const std::uint16_t def[12] = {0, 0, 0, 500, 50, 100, 0, 50, 50, 50, 80, 0};
  for (int i = 0; i < 12; ++i) {
    CHECK(p.send_fx().value[i] == def[i]);
  }
}

constexpr std::uint8_t kAllSession = Project::kStateNote | Project::kStateMute | Project::kStateScenes | Project::kStateSendFx;

}  // namespace

TEST_CASE("persist2: a fresh project has no session part, the defaults of the runtime objects, and a default file has no session chunk") {
  Project& p = proj(0);
  p.reset();
  CHECK((p.state_present() & kAllSession) == 0);
  check_defaults(p);
  CHECK(p.check_invariants());
  Bytes out;
  NEED(test::save(p, out) == Progress::Done);
  CHECK(out.size() == 186u);  // the default project is still the 186 bytes of docs/formats/o3-project.md section 4
  for (const RawChunk& c : test::split_chunks(out)) {
    CHECK_FALSE(is_session(c.id));
  }
  // the bits are the four that are left of the u8
  CHECK(Project::kStateNote == 16);
  CHECK(Project::kStateMute == 32);
  CHECK(Project::kStateScenes == 64);
  CHECK(Project::kStateSendFx == 128);
  CHECK((Project::kStateMidi | Project::kStateUsb | Project::kStateMix | Project::kStatePerf | kAllSession) == 255);
  // and a reset takes every part back
  test::fill_persist2(p);
  CHECK((p.state_present() & kAllSession) == kAllSession);
  p.reset();
  CHECK((p.state_present() & kAllSession) == 0);
  check_defaults(p);
}

TEST_CASE("persist2: set_* checks the value, marks the part present, and changes the generation only when something changed") {
  Project& p = proj(0);
  p.reset();
  // NOTE
  NoteState n;
  init_note_state(n);
  CHECK(validate_note_state(n) == nullptr);
  std::uint32_t gen = p.generation();
  CHECK(p.set_note(n) == Status::Ok);  // the defaults: the part becomes present (a chunk with the defaults is still a chunk), the generation moves once
  CHECK((p.state_present() & Project::kStateNote) != 0);
  CHECK(p.generation() != gen);
  gen = p.generation();
  CHECK(p.set_note(n) == Status::Ok);
  CHECK(p.generation() == gen);  // the same value of a part that is present: nothing
  for (int field = 0; field < 5; ++field) {
    NoteState bad = n;
    std::uint8_t* f[5] = {&bad.root, &bad.scale, &bad.octave, &bad.lock, &bad.jump};
    const std::uint8_t over[5] = {12, 39, 10, 2, 33};  // the scale's top is the official 38 (OS-SCALES28)
    *f[field] = over[field];
    CHECK(validate_note_state(bad) != nullptr);
    CHECK(p.set_note(bad) == Status::BadValue);
    CHECK(p.note() == n);
    CHECK(p.generation() == gen);
  }
  n.root = 11;
  n.scale = 38;  // the largest official scale (OS-SCALES28)
  n.octave = 9;
  n.lock = 1;
  n.jump = 32;
  CHECK(p.set_note(n) == Status::Ok);  // the largest of every field
  CHECK(p.note() == n);
  CHECK(p.generation() != gen);

  // MUTE
  p.reset();
  MuteState m;
  init_mute_state(m);
  m.mode[5] = 2;
  gen = p.generation();
  CHECK(p.set_mute(m) == Status::Ok);
  CHECK((p.state_present() & Project::kStateMute) != 0);
  CHECK(p.mute() == m);
  gen = p.generation();
  CHECK(p.set_mute(m) == Status::Ok);
  CHECK(p.generation() == gen);
  m.mode[15] = 3;
  CHECK(validate_mute_state(m) != nullptr);
  CHECK(p.set_mute(m) == Status::BadValue);
  CHECK(p.mute().mode[15] == 0);
  CHECK(p.generation() == gen);

  // SCEN
  p.reset();
  ScenesState s;
  init_scenes_state(s);
  CHECK(s.latch == 1);
  CHECK(validate_scenes_state(s) == nullptr);
  s.scene[1].stored = 1;
  init_perf_state(s.scene[1].slots);
  s.scene[1].slots.value[3][2] = -100;
  s.scene[1].slots.fx[3] = 19;
  s.scene[1].slots.row[3] = 3;
  CHECK(p.set_scenes(s) == Status::Ok);
  CHECK((p.state_present() & Project::kStateScenes) != 0);
  CHECK(p.scenes() == s);
  gen = p.generation();
  CHECK(p.set_scenes(s) == Status::Ok);
  CHECK(p.generation() == gen);
  const auto refused = [&](const char* what, auto edit) {
    INFO(what);
    ScenesState bad = s;
    edit(bad);
    CHECK(validate_scenes_state(bad) != nullptr);
    CHECK(p.set_scenes(bad) == Status::BadValue);
    CHECK(p.scenes() == s);
    CHECK(p.generation() == gen);
  };
  refused("latch 2", [](ScenesState& x) { x.latch = 2; });
  refused("reserved", [](ScenesState& x) { x.reserved = 1; });
  refused("stored 2", [](ScenesState& x) { x.scene[0].stored = 2; });
  refused("a scene's reserved byte", [](ScenesState& x) { x.scene[1].reserved = 1; });
  refused("a scene that is not stored holds a value", [](ScenesState& x) { x.scene[0].slots.fx[0] = 1; });
  refused("a scene that is not stored holds a value (the last byte)", [](ScenesState& x) { x.scene[2].slots.value[11][2] = 1; });
  refused("FX 22", [](ScenesState& x) { x.scene[1].slots.fx[0] = 22; });  // (20 and 21 are OS-PERFFX P1's BITDEPTH and OVERDRV)
  refused("row 4", [](ScenesState& x) { x.scene[1].slots.row[0] = 4; });
  refused("value 101", [](ScenesState& x) { x.scene[1].slots.value[0][0] = 101; });
  refused("value -101", [](ScenesState& x) { x.scene[1].slots.value[11][2] = -101; });

  // SNFX
  p.reset();
  SendFxState f;
  init_send_fx_state(f);
  CHECK(validate_send_fx_state(f) == nullptr);
  f.value[3] = 3500;
  f.value[11] = 1;
  CHECK(p.set_send_fx(f) == Status::Ok);
  CHECK((p.state_present() & Project::kStateSendFx) != 0);
  CHECK(p.send_fx() == f);
  gen = p.generation();
  CHECK(p.set_send_fx(f) == Status::Ok);
  CHECK(p.generation() == gen);
  const std::uint16_t lo[12] = {0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0};
  const std::uint16_t hi[12] = {1, 1, 18, 3500, 70, 100, 1, 100, 100, 100, 100, 1};
  for (int row = 0; row < 12; ++row) {
    SendFxState ok = f;
    ok.value[row] = lo[row];
    CHECK(validate_send_fx_state(ok) == nullptr);
    ok.value[row] = hi[row];
    CHECK(validate_send_fx_state(ok) == nullptr);
    SendFxState bad = f;
    bad.value[row] = static_cast<std::uint16_t>(hi[row] + 1);
    CHECK(validate_send_fx_state(bad) != nullptr);
    CHECK(p.set_send_fx(bad) == Status::BadValue);
    CHECK(p.send_fx() == f);
    if (lo[row] > 0) {
      bad.value[row] = static_cast<std::uint16_t>(lo[row] - 1);
      CHECK(validate_send_fx_state(bad) != nullptr);
    }
  }
  CHECK(p.generation() == gen);
  CHECK(p.check_invariants());
}

TEST_CASE("persist2: the Editor's wrappers keep the undo history, the Project's own setters end it") {
  alignas(8) static std::uint8_t ring[64 * 1024];
  Project& p = proj(0);
  p.reset();
  UndoJournal journal;
  NEED(journal.begin(ring, sizeof ring));
  Editor ed(p, journal);
  CHECK(ed.create_pattern(1, 16) == Status::Ok);
  CHECK(ed.can_undo());
  NoteState n;
  init_note_state(n);
  n.scale = 7;  // Phrygian, the official index (OS-SCALES28)
  CHECK(ed.set_note_state(n) == Status::Ok);
  MuteState m;
  init_mute_state(m);
  m.mode[1] = 1;
  CHECK(ed.set_mute_state(m) == Status::Ok);
  ScenesState s;
  init_scenes_state(s);
  s.latch = 0;
  CHECK(ed.set_scenes_state(s) == Status::Ok);
  SendFxState f;
  init_send_fx_state(f);
  f.value[5] = 40;
  CHECK(ed.set_send_fx_state(f) == Status::Ok);
  CHECK(ed.set_send_fx_state(f) == Status::Ok);  // unchanged: nothing
  CHECK(ed.can_undo());                          // the history is still the history of the pattern
  CHECK(ed.undo() == UndoResult::Ok);
  CHECK(p.pattern(1) == nullptr);
  CHECK(p.note().scale == 7);  // the state is not part of any undo unit
  CHECK(p.mute().mode[1] == 1);
  CHECK(p.scenes().latch == 0);
  CHECK(p.send_fx().value[5] == 40);
  CHECK(ed.redo() == UndoResult::Ok);
  CHECK(ed.can_undo());
  // a change behind the editor's back ends the history: that is what the wrappers are for
  n.scale = 4;  // Lydian, the official index (OS-SCALES28)
  CHECK(p.set_note(n) == Status::Ok);
  CHECK_FALSE(ed.can_undo());
  // a history that was already stale stays so
  n.scale = 9;  // Mixolydian, the official index
  CHECK(ed.set_note_state(n) == Status::Ok);
  CHECK_FALSE(ed.can_undo());
  NoteState bad = n;
  bad.root = 12;
  CHECK(ed.set_note_state(bad) == Status::BadValue);
  MuteState badm = m;
  badm.mode[0] = 3;
  CHECK(ed.set_mute_state(badm) == Status::BadValue);
  SendFxState badf = f;
  badf.value[3] = 0;
  CHECK(ed.set_send_fx_state(badf) == Status::BadValue);
}

TEST_CASE("persist2: the chunks are the bytes of the description, and the file round-trips (save, load, save)") {
  const Bytes file = session_file();
  const std::vector<RawChunk> cs = test::split_chunks(file);
  // NOTE, MUTE, SCEN and SNFX come after the known chunks (here without the OS-004d state) and before the unknown ones, in that order.
  std::vector<std::uint32_t> order;
  for (const RawChunk& c : cs) {
    order.push_back(c.id);
  }
  std::size_t at = 0;
  while (at < order.size() && order[at] != kChunkNote) {
    ++at;
  }
  NEED(at + 4 < order.size());
  CHECK(order[at + 1] == kChunkMute);
  CHECK(order[at + 2] == kChunkScen);
  CHECK(order[at + 3] == kChunkSnfx);
  CHECK(order[at - 1] == kChunkInst);
  CHECK(order[at + 4] == test::split_chunks(file)[at + 4].id);
  CHECK(order.size() == at + 4 + 2);  // the two unknown chunks of the small fixture follow
  CHECK(find(cs, kChunkNote)->payload == expected_note());
  CHECK(find(cs, kChunkMute)->payload == expected_mute());
  CHECK(find(cs, kChunkScen)->payload == expected_scen());
  CHECK(find(cs, kChunkSnfx)->payload == expected_snfx());
  CHECK(find(cs, kChunkNote)->payload.size() == 7u);
  CHECK(find(cs, kChunkMute)->payload.size() == 19u);
  CHECK(find(cs, kChunkScen)->payload.size() == 295u);
  CHECK(find(cs, kChunkSnfx)->payload.size() == 27u);
  // the ids are four printable ASCII characters
  CHECK(kChunkNote == fourcc('N', 'O', 'T', 'E'));
  CHECK(kChunkMute == fourcc('M', 'U', 'T', 'E'));
  CHECK(kChunkScen == fourcc('S', 'C', 'E', 'N'));
  CHECK(kChunkSnfx == fourcc('S', 'N', 'F', 'X'));

  Report rep;
  NEED(test::load(test::arena(1), file, &rep) == Progress::Done);
  CHECK_FALSE(rep.newer_minor);
  CHECK(rep.settings_skipped == 0u);
  Project& q = proj(1);
  CHECK((q.state_present() & kAllSession) == kAllSession);
  CHECK(q.note() == proj(2).note());
  CHECK(q.mute() == proj(2).mute());
  CHECK(q.scenes() == proj(2).scenes());
  CHECK(q.send_fx() == proj(2).send_fx());
  // field by field, so a swapped pair cannot hide behind the equality
  CHECK(q.note().root == 7);
  CHECK(q.note().scale == 15);  // Blues, the official index (OS-SCALES28); the 1.0 byte was 9
  CHECK(q.note().octave == 5);
  CHECK(q.note().lock == 1);
  CHECK(q.note().jump == 4);
  CHECK(q.mute().mode[0] == 1);
  CHECK(q.mute().mode[3] == 2);
  CHECK(q.mute().mode[15] == 1);
  CHECK(q.scenes().latch == 0);
  CHECK(q.scenes().scene[0].stored == 1);
  CHECK(q.scenes().scene[1].stored == 0);
  CHECK(q.scenes().scene[2].stored == 1);
  CHECK(q.scenes().scene[0].slots.fx[11] == 19);
  CHECK(q.scenes().scene[0].slots.row[4] == 3);
  CHECK(q.scenes().scene[0].slots.value[4][2] == -48);
  CHECK(q.scenes().scene[2].slots.value[11][0] == 15);
  CHECK(q.send_fx().value[3] == 3500);
  CHECK(q.send_fx().value[11] == 1);
  CHECK(q.check_invariants());
  Bytes again;
  NEED(test::save(q, again) == Progress::Done);
  CHECK(again == file);
  Bytes small_budget;
  NEED(test::save(q, small_budget, 7) == Progress::Done);  // any step budget gives the same bytes
  CHECK(small_budget == file);
  // with the OS-004d state next to it, too
  test::fill_state(proj(0));
  CHECK(proj(0).set_note(proj(2).note()) == Status::Ok);
  CHECK(proj(0).set_mute(proj(2).mute()) == Status::Ok);
  CHECK(proj(0).set_scenes(proj(2).scenes()) == Status::Ok);
  CHECK(proj(0).set_send_fx(proj(2).send_fx()) == Status::Ok);
  Bytes both;
  NEED(test::save(proj(0), both) == Progress::Done);
  std::vector<std::uint32_t> ids;
  for (const RawChunk& c : test::split_chunks(both)) {
    ids.push_back(c.id);
  }
  const std::uint32_t want[] = {kChunkMidi, kChunkUsbs, kChunkMixr, kChunkPerf, kChunkAppm, kChunkNote, kChunkMute, kChunkScen, kChunkSnfx};
  std::size_t first = 0;
  while (first < ids.size() && ids[first] != kChunkMidi) {
    ++first;
  }
  NEED(first + 9 <= ids.size());
  for (int i = 0; i < 9; ++i) {
    CHECK(ids[first + static_cast<std::size_t>(i)] == want[i]);
  }
  NEED(test::load(test::arena(1), both, &rep) == Progress::Done);
  Bytes both_again;
  NEED(test::save(proj(1), both_again) == Progress::Done);
  CHECK(both_again == both);
}

// OS-PERFFX P1, decision D5 (option A): a scene snapshots the slots, so it holds the new ids too (the SCEN chunk is a PERF slot's layout 3 x 12 times, the same byte for the id). The chunk keeps its 295
// bytes; its VERSION is 1.1 only when a stored scene holds the id 20 or 21 and 1.0, byte for byte, otherwise (the pattern of INST 1.2).
TEST_CASE("persist2 P1: SCEN is 1.0 byte for byte while the ids 20 and 21 are unused, 1.1 when a stored scene holds one, and both versions load") {
  // a project whose scenes hold ids up to 19 is the chunk it always was
  {
    test::fill_persist2(proj(2));
    Bytes plain;
    NEED(test::save(proj(2), plain) == Progress::Done);
    const std::vector<RawChunk> plain_cs = test::split_chunks(plain);
    NEED(find(plain_cs, kChunkScen) != nullptr);
    CHECK(find(plain_cs, kChunkScen)->payload == expected_scen());  // (0x0100 in front)
    CHECK(find(plain_cs, kChunkScen)->payload[1] == 0x00);
  }
  test::fill_persist2(proj(2));
  ScenesState sc = proj(2).scenes();
  sc.scene[1].stored = 1;
  init_perf_state(sc.scene[1].slots);
  sc.scene[1].slots.fx[0] = 20;  // BITDEPTH
  sc.scene[1].slots.row[0] = 2;
  sc.scene[1].slots.value[0][1] = 15;
  sc.scene[1].slots.fx[9] = 21;  // OVERDRV
  sc.scene[1].slots.row[9] = 3;
  sc.scene[1].slots.value[9][2] = 100;
  sc.scene[0].slots.fx[1] = 21;
  sc.scene[0].slots.value[1][0] = 1;
  CHECK(validate_scenes_state(sc) == nullptr);
  CHECK(proj(2).set_scenes(sc) == Status::Ok);
  Bytes file;
  NEED(test::save(proj(2), file) == Progress::Done);
  const std::vector<RawChunk> saved = test::split_chunks(file);
  const RawChunk* const scen = find(saved, kChunkScen);
  NEED(scen != nullptr);
  CHECK(scen->payload.size() == 295u);
  CHECK(scen->payload[0] == 0x01);  // version 1.1
  CHECK(scen->payload[1] == 0x01);
  const std::size_t scene1 = 4u + 97u;  // the version, LATCH and the count, then scene 0 of 97 bytes
  CHECK(scen->payload[scene1] == 1);
  CHECK(scen->payload[scene1 + 1] == 20);
  CHECK(scen->payload[scene1 + 2] == 2);
  CHECK(scen->payload[scene1 + 1 + 9 * 8] == 21);  // slot 9: a scene is a flag and 12 slots of 8 bytes
  CHECK(scen->payload[4 + 1 + 1 * 8] == 21);
  Report rep;
  NEED(test::load(test::arena(1), file, &rep) == Progress::Done);  // a 1.1 file loads
  CHECK_FALSE(rep.newer_minor);
  CHECK(rep.settings_skipped == 0u);
  CHECK(proj(1).scenes() == sc);
  Bytes again;
  NEED(test::save(proj(1), again) == Progress::Done);
  CHECK(again == file);
  // 20 and 21 are valid only in 1.1: in a 1.0 chunk they read as none, row and values reset (an older build's reading of every 1.1 file) and are counted
  {
    std::vector<RawChunk> cs = test::split_chunks(file);
    for (RawChunk& c : cs) {
      if (c.id == kChunkScen) {
        c.payload[1] = 0x00;
      }
    }
    NEED(test::load(test::arena(1), build(cs), &rep) == Progress::Done);
    CHECK(rep.settings_skipped == 3u);  // the three slots
    const ScenesState& q = proj(1).scenes();
    CHECK(q.scene[1].slots.fx[0] == 0);
    CHECK(q.scene[1].slots.row[0] == 0);
    CHECK(q.scene[1].slots.value[0][1] == 0);
    CHECK(q.scene[1].slots.fx[9] == 0);
    CHECK(q.scene[1].slots.value[9][2] == 0);
    CHECK(q.scene[0].slots.fx[1] == 0);
    CHECK(q.scene[0].slots.fx[0] == sc.scene[0].slots.fx[0]);  // the others are as the file said
    CHECK(validate_scenes_state(q) == nullptr);
    Bytes saved_again;
    NEED(test::save(proj(1), saved_again) == Progress::Done);
    const std::vector<RawChunk> sa = test::split_chunks(saved_again);
    NEED(find(sa, kChunkScen) != nullptr);
    CHECK(find(sa, kChunkScen)->payload[1] == 0x00);  // and it writes 1.0
  }
  // in 1.1 an id above 21 is still none (the others stay); a 1.1 chunk with neither id loads and is written back as 1.0
  {
    std::vector<RawChunk> cs = test::split_chunks(file);
    for (RawChunk& c : cs) {
      if (c.id == kChunkScen) {
        c.payload[scene1 + 1] = 22;
      }
    }
    NEED(test::load(test::arena(1), build(cs), &rep) == Progress::Done);
    CHECK(proj(1).scenes().scene[1].slots.fx[0] == 0);
    CHECK(proj(1).scenes().scene[1].slots.row[0] == 0);
    CHECK(proj(1).scenes().scene[1].slots.value[0][1] == 0);
    CHECK(proj(1).scenes().scene[1].slots.fx[9] == 21);  // the others are as the file said
    NEED(test::load(test::arena(1), file, &rep) == Progress::Done);
    ScenesState plain = proj(1).scenes();
    plain.scene[1].slots.fx[0] = 3;
    plain.scene[1].slots.fx[9] = 4;
    plain.scene[0].slots.fx[1] = 5;
    CHECK(proj(1).set_scenes(plain) == Status::Ok);
    Bytes b19;
    NEED(test::save(proj(1), b19) == Progress::Done);
    std::vector<RawChunk> cs19 = test::split_chunks(b19);
    for (RawChunk& c : cs19) {
      if (c.id == kChunkScen) {
        CHECK(c.payload[1] == 0x00);
        c.payload[1] = 0x01;
      }
    }
    NEED(test::load(test::arena(1), build(cs19), &rep) == Progress::Done);
    CHECK_FALSE(rep.newer_minor);
    CHECK(rep.settings_skipped == 0u);
    CHECK(proj(1).scenes() == plain);
    Bytes b19_again;
    NEED(test::save(proj(1), b19_again) == Progress::Done);
    CHECK(b19_again == b19);
  }
}

TEST_CASE("persist2: every field survives a round trip at both ends of its range and over a few hundred random states") {
  test::Rng r(20261013);
  Project& p = proj(0);
  for (int round = 0; round < 300; ++round) {
    p.reset();
    NoteState n;
    MuteState m;
    ScenesState s;
    SendFxState f;
    const bool edge = round < 2;  // round 0: every field at its smallest, round 1 at its largest
    n.root = static_cast<std::uint8_t>(edge ? (round == 0 ? 0 : 11) : r.next() % 12);
    n.scale = static_cast<std::uint8_t>(edge ? (round == 0 ? 0 : 38) : r.next() % 39);  // the official scales 0..38 (OS-SCALES28)
    n.octave = static_cast<std::uint8_t>(edge ? (round == 0 ? 0 : 9) : r.next() % 10);
    n.lock = static_cast<std::uint8_t>(edge ? round : r.next() % 2);
    n.jump = static_cast<std::uint8_t>(edge ? (round == 0 ? 0 : 32) : r.next() % 33);
    for (std::uint8_t& v : m.mode) {
      v = static_cast<std::uint8_t>(edge ? (round == 0 ? 0 : 2) : r.next() % 3);
    }
    std::memset(&s, 0, sizeof s);
    s.latch = static_cast<std::uint8_t>(edge ? round : r.next() % 2);
    for (SceneState& sc : s.scene) {
      sc.stored = static_cast<std::uint8_t>(edge ? round : r.next() % 2);
      if (sc.stored == 0) {
        continue;
      }
      for (std::uint32_t i = 0; i < 12; ++i) {
        sc.slots.fx[i] = static_cast<std::uint8_t>(edge ? (round == 0 ? 0 : 21) : r.next() % 22);  // (0..21: OS-PERFFX P1 added 20 and 21)
        sc.slots.row[i] = static_cast<std::uint8_t>(edge ? (round == 0 ? 0 : 3) : r.next() % 4);
        for (std::int16_t& v : sc.slots.value[i]) {
          // OS-PERFMODES M3: a PLAYMODE (id 18) slot holds 0..23 (4..23 are the custom play orders, written as SCEN 1.2 and read as they are; in a 1.0 / 1.1 chunk a value above 3 reads as RND, 3)
          if (sc.slots.fx[i] == 18) {
            v = static_cast<std::int16_t>(edge ? (round == 0 ? 0 : 23) : r.next() % 24);
          } else {
            v = static_cast<std::int16_t>(edge ? (round == 0 ? -100 : 100) : static_cast<int>(r.next() % 201) - 100);
          }
        }
      }
    }
    const std::uint16_t lo[12] = {0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    const std::uint16_t hi[12] = {1, 1, 18, 3500, 70, 100, 1, 100, 100, 100, 100, 1};
    for (int i = 0; i < 12; ++i) {
      f.value[i] = edge ? (round == 0 ? lo[i] : hi[i]) : static_cast<std::uint16_t>(lo[i] + r.next() % (hi[i] - lo[i] + 1u));
    }
    NEED(p.set_note(n) == Status::Ok);
    NEED(p.set_mute(m) == Status::Ok);
    NEED(p.set_scenes(s) == Status::Ok);
    NEED(p.set_send_fx(f) == Status::Ok);
    Bytes file;
    NEED(test::save(p, file) == Progress::Done);
    Report rep;
    NEED(test::load(test::arena(1), file, &rep) == Progress::Done);
    CHECK(rep.settings_skipped == 0u);
    Project& q = proj(1);
    NEED(q.note() == n);
    NEED(q.mute() == m);
    NEED(q.scenes() == s);
    NEED(q.send_fx() == f);
    Bytes again;
    NEED(test::save(q, again) == Progress::Done);
    NEED(again == file);
  }
}

TEST_CASE("persist2: a part with the default values is still a chunk (the part is present), and the writer writes only what is present") {
  Project& p = proj(0);
  p.reset();
  NoteState n;
  init_note_state(n);
  CHECK(p.set_note(n) == Status::Ok);
  Bytes out;
  NEED(test::save(p, out) == Progress::Done);
  std::vector<RawChunk> cs = test::split_chunks(out);
  CHECK(find(cs, kChunkNote) != nullptr);
  CHECK(find(cs, kChunkMute) == nullptr);
  CHECK(find(cs, kChunkScen) == nullptr);
  CHECK(find(cs, kChunkSnfx) == nullptr);
  CHECK(find(cs, kChunkNote)->payload == Bytes({0x01, 0x00, 0, 0, 3, 0, 1}));
  CHECK(out.size() == 186u + 8u + 7u + 1u);  // header, 7 bytes, a pad
  Report rep;
  NEED(test::load(test::arena(1), out, &rep) == Progress::Done);
  CHECK(proj(1).state_present() == Project::kStateNote);  // the file said it
  check_defaults(proj(1));
  // each of the others alone
  p.reset();
  MuteState m;
  init_mute_state(m);
  CHECK(p.set_mute(m) == Status::Ok);
  NEED(test::save(p, out) == Progress::Done);
  cs = test::split_chunks(out);
  CHECK(find(cs, kChunkMute)->payload == Bytes({0x01, 0x00, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
  CHECK(cs.size() == 5u);  // PHDR, TRAK, MODR, SONG, MUTE
  p.reset();
  ScenesState s;
  init_scenes_state(s);
  CHECK(p.set_scenes(s) == Status::Ok);
  NEED(test::save(p, out) == Progress::Done);
  cs = test::split_chunks(out);
  NEED(find(cs, kChunkScen) != nullptr);
  CHECK(find(cs, kChunkScen)->payload.size() == 295u);
  CHECK(find(cs, kChunkScen)->payload[2] == 1);  // LATCH on
  CHECK(find(cs, kChunkScen)->payload[3] == 3);
  p.reset();
  SendFxState f;
  init_send_fx_state(f);
  CHECK(p.set_send_fx(f) == Status::Ok);
  NEED(test::save(p, out) == Progress::Done);
  cs = test::split_chunks(out);
  CHECK(find(cs, kChunkSnfx)->payload == Bytes({0x01, 0x00, 12, 0, 0, 0, 0, 0, 0, 0x01, 0xF4, 0, 50, 0, 100, 0, 0, 0, 50, 0, 50, 0, 50, 0, 80, 0, 0}));
}

TEST_CASE("persist2: an older file (none of the four chunks) reads as the defaults and is written back as it was, and the writer is byte for byte the previous build's") {
  // The projects of the fixtures that have none of the four, written by the build BEFORE OS-PERSIST2 (d96b7ef), are the files below: their size and CRC-32 were taken from that build's
  // writer (o3pj_tool gen small / state). The writer of this build must produce the same bytes (a project that never touched the session state is the file it always was), and the
  // reader must read them as the defaults and write them back unchanged.
  struct Old {
    std::uint32_t size;
    std::uint32_t crc;
  };
  // OS-INSTUI S6: a fresh instrument stores our LFO defaults (triangle, 0, 0.5: init_instrument), so the two files that hold instruments differ from the previous build's in those bytes only (the size is the
  // same): their CRC-32 below are of this build's writer; the empty project has no instrument and keeps the old one.
  const Old small_old = {2648, 0x3f04f4e7u};
  const Old state_old = {3372, 0xa8d283e1u};
  const Old empty_old = {186, 0xE8816302u};
  const auto check = [](const char* what, Project& src, const Old& old) {
    INFO(what);
    Bytes file;
    NEED(test::save(src, file) == Progress::Done);
    CHECK(file.size() == old.size);
    CHECK(formats::crc32(file.data(), file.size()) == old.crc);
    for (const RawChunk& c : test::split_chunks(file)) {
      CHECK_FALSE(is_session(c.id));
    }
    Report rep;
    NEED(test::load(test::arena(1), file, &rep) == Progress::Done);
    CHECK_FALSE(rep.newer_minor);
    CHECK(rep.settings_skipped == 0u);
    CHECK((proj(1).state_present() & kAllSession) == 0);
    check_defaults(proj(1));
    CHECK(proj(1).check_invariants());
    Bytes back;
    NEED(test::save(proj(1), back) == Progress::Done);
    CHECK(back == file);
  };
  proj(0).reset();
  check("the empty project", proj(0), empty_old);
  test::fill_small(proj(0));
  check("the small project", proj(0), small_old);
  test::fill_state(proj(0));
  check("the project with the state of OS-004d", proj(0), state_old);

  // and a file of the new writer with the four chunks taken out is the older file
  const Bytes with = session_file();
  std::vector<RawChunk> cs = test::split_chunks(with);
  std::vector<RawChunk> without;
  for (const RawChunk& c : cs) {
    if (!is_session(c.id)) {
      without.push_back(c);
    }
  }
  test::fill_small(proj(0));
  Bytes small_file;
  NEED(test::save(proj(0), small_file) == Progress::Done);
  CHECK(build(without) == small_file);
  CHECK(small_file.size() == small_old.size);
}

TEST_CASE("persist2: the previous build keeps the four chunks as unknown chunks: the pool of an older build holds them next to every other chunk and a full OG import") {
  // docs/formats/o3-project.md section 3: an older build (the one before OS-PERSIST2) knows none of NOTE, MUTE, SCEN, SNFX: they are unknown chunks to it (16 chunks, 16,384 bytes) that it
  // writes back as it read them. At their largest they are 8 + 19 + 295 + 27 = 349 bytes in four chunks (OS-SCALES28: the NOTE 1.1 chunk is 8); the OS-004d chunks are 1,152 in five; the OG chunks
  // that share the pool are OGPM (at most 970) and OGPN (at most 12,822).
  constexpr std::size_t session_max = 8 + 19 + 295 + 27;
  constexpr std::size_t state_max = 20 + 27 + 25 + 99 + 981;
  CHECK(session_max == 349u);
  CHECK(state_max == 1152u);
  CHECK(session_max + state_max + 970u + 12822u <= kUnknownPoolBytes);
  CHECK(session_max + state_max + 970u + 12822u == 15293u);
  CHECK(4u + 5u + 2u <= kMaxUnknownChunks);
  // OS-BUG-PASTE24: the LAW chunk (3 bytes, one more chunk) joins them for an OG project below version 7: 15,296 bytes in 12 chunks (law_format_test.cpp)
  constexpr std::size_t law_max = 3;
  CHECK(law_max + session_max + state_max + 970u + 12822u == 15296u);
  CHECK(1u + 4u + 5u + 2u == 12u);
  CHECK(1u + 4u + 5u + 2u <= kMaxUnknownChunks);
  // the writer's chunks are at most those sizes: the persist2 fixture's NOTE is 1.0 (7 bytes), the scales fixture's is 1.1 (8)
  const std::vector<RawChunk> cs = test::split_chunks(session_file());
  CHECK(find(cs, kChunkNote)->payload.size() + find(cs, kChunkMute)->payload.size() + find(cs, kChunkScen)->payload.size() + find(cs, kChunkSnfx)->payload.size() == session_max - 1u);
  test::fill_scales(proj(0));
  Bytes scales_file;
  NEED(test::save(proj(0), scales_file) == Progress::Done);
  const std::vector<RawChunk> sc = test::split_chunks(scales_file);
  CHECK(find(sc, kChunkNote)->payload.size() + find(sc, kChunkMute)->payload.size() + find(sc, kChunkScen)->payload.size() + find(sc, kChunkSnfx)->payload.size() == session_max);
}

namespace {

// Loads `edit` of the persist2 fixture; the file must load (Done) with `skipped` values reset.
struct Tolerant {
  std::vector<RawChunk> cs = test::split_chunks(session_file());
  Report rep;

  template <typename Fn>
  Project& load_with(std::uint32_t id, Fn fn) {
    std::vector<RawChunk> out = cs;
    for (RawChunk& c : out) {
      if (c.id == id) {
        fn(c.payload);
      }
    }
    const Bytes file = build(out);
    CHECK(test::load(test::arena(1), file, &rep) == Progress::Done);
    CHECK(proj(1).check_invariants());
    CHECK(rep.error == Error::None);
    return proj(1);
  }
};

}  // namespace

TEST_CASE("persist2: the reader is tolerant: a value out of range is its default and is counted, the load goes on") {
  Tolerant t;
  // NOTE
  {
    Project& q = t.load_with(kChunkNote, [](Bytes& p) { p[2] = 12; });
    CHECK(q.note().root == 0);
    CHECK(q.note().scale == 15);  // the others are what the file said (the 1.0 byte 9 = Blues = the official 15)
    CHECK(q.note().octave == 5);
    CHECK(t.rep.settings_skipped == 1u);
  }
  {
    Project& q = t.load_with(kChunkNote, [](Bytes& p) { p[3] = 12; });  // scale10 12: above the twelve, Chromatic
    CHECK(q.note().scale == 0);
    CHECK(q.note().root == 7);
    CHECK(t.rep.settings_skipped == 1u);
  }
  {
    Project& q = t.load_with(kChunkNote, [](Bytes& p) { p[3] = 11; });  // scale10 11: the twelfth pad, Whole tone = the official 23 (OS-SCALES28)
    CHECK(q.note().scale == 23);
    CHECK(t.rep.settings_skipped == 0u);
  }
  {
    Project& q = t.load_with(kChunkNote, [](Bytes& p) { p[4] = 10; });
    CHECK(q.note().octave == 3);
    CHECK(t.rep.settings_skipped == 1u);
  }
  {
    Project& q = t.load_with(kChunkNote, [](Bytes& p) { p[5] = 2; });
    CHECK(q.note().lock == 0);
    CHECK(t.rep.settings_skipped == 1u);
  }
  {
    Project& q = t.load_with(kChunkNote, [](Bytes& p) { p[6] = 33; });
    CHECK(q.note().jump == 1);
    CHECK(t.rep.settings_skipped == 1u);
  }
  {
    Project& q = t.load_with(kChunkNote, [](Bytes& p) { for (int i = 2; i < 7; ++i) p[static_cast<std::size_t>(i)] = 0xFF; });
    NoteState def;
    init_note_state(def);
    CHECK(q.note() == def);
    CHECK((q.state_present() & Project::kStateNote) != 0);  // the file said it: the part is present, with what was taken
    CHECK(t.rep.settings_skipped == 5u);
  }
  {
    Project& q = t.load_with(kChunkNote, [](Bytes& p) { p[2] = 11; p[3] = 11; p[4] = 9; p[5] = 1; p[6] = 32; });  // the edges are valid (scale10 11 = the official 23)
    CHECK(q.note().root == 11);
    CHECK(q.note().scale == 23);
    CHECK(q.note().octave == 9);
    CHECK(q.note().lock == 1);
    CHECK(q.note().jump == 32);
    CHECK(t.rep.settings_skipped == 0u);
  }
  // MUTE
  {
    Project& q = t.load_with(kChunkMute, [](Bytes& p) { p[3 + 3] = 3; p[3 + 15] = 255; });
    CHECK(q.mute().mode[3] == 0);
    CHECK(q.mute().mode[15] == 0);
    CHECK(q.mute().mode[0] == 1);
    CHECK(t.rep.settings_skipped == 2u);
  }
  {
    Project& q = t.load_with(kChunkMute, [](Bytes& p) { p.resize(3 + 4); p[2] = 4; });  // four tracks: the others are Immediate
    CHECK(q.mute().mode[0] == 1);
    CHECK(q.mute().mode[3] == 2);
    CHECK(q.mute().mode[15] == 0);
    CHECK(t.rep.settings_skipped == 0u);
  }
  {
    Project& q = t.load_with(kChunkMute, [](Bytes& p) { p.resize(3 + 1); p[2] = 1; });
    CHECK(q.mute().mode[0] == 1);
    CHECK(q.mute().mode[3] == 0);
    CHECK(t.rep.settings_skipped == 0u);
  }
  // SCEN
  {
    Project& q = t.load_with(kChunkScen, [](Bytes& p) { p[2] = 2; });  // LATCH 2: on
    CHECK(q.scenes().latch == 1);
    CHECK(q.scenes().scene[0].stored == 1);
    CHECK(t.rep.settings_skipped == 1u);
  }
  {
    Project& q = t.load_with(kChunkScen, [](Bytes& p) { p[4] = 2; });  // stored 2: a scene this version does not know: not stored, and its bytes mean nothing
    CHECK(q.scenes().scene[0].stored == 0);
    CHECK(q.scenes().scene[0].slots.fx[0] == 0);
    CHECK(q.scenes().scene[0].slots.value[0][0] == 0);
    CHECK(q.scenes().scene[2].stored == 1);
    CHECK(t.rep.settings_skipped == 1u);
    CHECK(validate_scenes_state(q.scenes()) == nullptr);
  }
  {
    Project& q = t.load_with(kChunkScen, [](Bytes& p) {  // junk in a scene that is not stored: ignored, not counted
      for (std::size_t i = 4 + 97 + 1; i < 4 + 2 * 97; ++i) p[i] = 0x55;
    });
    CHECK(q.scenes().scene[1].stored == 0);
    const std::uint8_t* b = reinterpret_cast<const std::uint8_t*>(&q.scenes().scene[1]);
    bool zero = true;
    for (std::size_t i = 0; i < sizeof(SceneState); ++i) {
      zero = zero && b[i] == 0;
    }
    CHECK(zero);  // what is kept of it is zeros
    CHECK(t.rep.settings_skipped == 0u);
    CHECK(q.scenes() == proj(2).scenes());
  }
  {
    Project& q = t.load_with(kChunkScen, [](Bytes& p) { p[4 + 1] = 22; });  // slot 0 of scene 0: FX 22 (an FX a later version added): none, row and values reset
    CHECK(q.scenes().scene[0].slots.fx[0] == 0);
    CHECK(q.scenes().scene[0].slots.row[0] == 0);
    CHECK(q.scenes().scene[0].slots.value[0][0] == 0);
    CHECK(q.scenes().scene[0].slots.value[0][1] == 0);
    CHECK(q.scenes().scene[0].slots.value[0][2] == 0);
    CHECK(q.scenes().scene[0].slots.fx[11] == 19);  // the others are what the file said
    CHECK(t.rep.settings_skipped == 1u);
  }
  {
    Project& q = t.load_with(kChunkScen, [](Bytes& p) { p[4 + 2] = 4; });  // row 4: off
    CHECK(q.scenes().scene[0].slots.fx[0] == 1);
    CHECK(q.scenes().scene[0].slots.row[0] == 0);
    CHECK(q.scenes().scene[0].slots.value[0][0] == -100);
    CHECK(t.rep.settings_skipped == 1u);
  }
  {
    Project& q = t.load_with(kChunkScen, [](Bytes& p) {
      p[4 + 3] = 0;    // v0 = 101
      p[4 + 4] = 101;
      p[4 + 5] = 0xFF;  // v1 = -101
      p[4 + 6] = 0x9B;
      p[4 + 7] = 0x7F;  // v2 = 32767
      p[4 + 8] = 0xFF;
    });
    CHECK(q.scenes().scene[0].slots.value[0][0] == 0);
    CHECK(q.scenes().scene[0].slots.value[0][1] == 0);
    CHECK(q.scenes().scene[0].slots.value[0][2] == 0);
    CHECK(q.scenes().scene[0].slots.row[0] == 2);  // the row stays
    CHECK(t.rep.settings_skipped == 3u);
  }
  {
    Project& q = t.load_with(kChunkScen, [](Bytes& p) {  // the edges are valid: 100, -100, 0 and FX 19 on row 3
      p[4 + 3] = 0;
      p[4 + 4] = 100;
      p[4 + 5] = 0xFF;
      p[4 + 6] = 0x9C;
    });
    CHECK(q.scenes().scene[0].slots.value[0][0] == 100);
    CHECK(q.scenes().scene[0].slots.value[0][1] == -100);
    CHECK(t.rep.settings_skipped == 0u);
  }
  // SNFX
  {
    Project& q = t.load_with(kChunkSnfx, [](Bytes& p) { p[3 + 2 * 2] = 0; p[3 + 2 * 2 + 1] = 19; });  // DLY RATE 19
    CHECK(q.send_fx().value[2] == 0);
    CHECK(q.send_fx().value[3] == 3500);
    CHECK(t.rep.settings_skipped == 1u);
  }
  {
    Project& q = t.load_with(kChunkSnfx, [](Bytes& p) { p[3 + 2 * 3] = 0; p[3 + 2 * 3 + 1] = 0; });  // DLY TIME 0 (the range starts at 1): the default, 500
    CHECK(q.send_fx().value[3] == 500);
    CHECK(t.rep.settings_skipped == 1u);
  }
  {
    Project& q = t.load_with(kChunkSnfx, [](Bytes& p) { p[3 + 2 * 3] = 0x0D; p[3 + 2 * 3 + 1] = 0xAD; });  // 3501
    CHECK(q.send_fx().value[3] == 500);
    CHECK(t.rep.settings_skipped == 1u);
  }
  {
    Project& q = t.load_with(kChunkSnfx, [](Bytes& p) { p[3 + 2 * 4 + 1] = 71; });  // DLY FEEDBACK 71: the page's top is 70
    CHECK(q.send_fx().value[4] == 50);
    CHECK(t.rep.settings_skipped == 1u);
  }
  {
    Project& q = t.load_with(kChunkSnfx, [](Bytes& p) {
      for (std::size_t i = 3; i < 3 + 24; ++i) p[i] = 0xFF;  // every row 65535
    });
    SendFxState def;
    init_send_fx_state(def);
    CHECK(q.send_fx() == def);
    CHECK(t.rep.settings_skipped == 12u);
  }
  {
    Project& q = t.load_with(kChunkSnfx, [](Bytes& p) { p.resize(3 + 10); p[2] = 5; });  // five rows: the others are their defaults, not zero
    CHECK(q.send_fx().value[0] == 1);
    CHECK(q.send_fx().value[4] == 70);
    CHECK(q.send_fx().value[5] == 100);  // DLY TONE
    CHECK(q.send_fx().value[7] == 50);
    CHECK(q.send_fx().value[10] == 80);
    CHECK(t.rep.settings_skipped == 0u);
  }
  {
    Project& q = t.load_with(kChunkSnfx, [](Bytes& p) { p[2] = 14; p.push_back(0); p.push_back(7); p.push_back(0); p.push_back(9); });  // two rows of a later version
    CHECK(q.send_fx() == proj(2).send_fx());
    CHECK(t.rep.settings_skipped == 2u);
  }
  {
    Project& q = t.load_with(kChunkSnfx, [](Bytes& p) { p[2] = 32; for (int i = 0; i < 40; ++i) p.push_back(static_cast<std::uint8_t>(i & 1)); });
    CHECK(q.send_fx() == proj(2).send_fx());
    CHECK(t.rep.settings_skipped == 20u);
  }
  // what was taken is written back, so the next save is the clean file
  t.load_with(kChunkNote, [](Bytes& p) { p[2] = 12; });
  Bytes resaved;
  NEED(test::save(proj(1), resaved) == Progress::Done);
  const std::vector<RawChunk> back = test::split_chunks(resaved);
  CHECK(find(back, kChunkNote)->payload == Bytes({0x01, 0x00, 0, 9, 5, 1, 4}));
  // and a few at once, in different chunks, add up
  {
    std::vector<RawChunk> out = t.cs;
    for (RawChunk& c : out) {
      if (c.id == kChunkNote) c.payload[3] = 99;
      if (c.id == kChunkMute) c.payload[3] = 9;
      if (c.id == kChunkSnfx) c.payload[4] = 200;
    }
    NEED(test::load(test::arena(1), build(out), &t.rep) == Progress::Done);
    CHECK(t.rep.settings_skipped == 3u);
  }
}

TEST_CASE("persist2: the registry's report of a native project says how many values were reset (FormatReport::sanitised), and none for a clean file or the writer") {
  static formats::BuiltinFormats storage;
  formats::FormatRegistry registry;
  NEED(formats::register_builtin_formats(registry, storage));
  const formats::FormatDesc* native = registry.find_id("native");
  NEED(native != nullptr);
  const auto read = [&](const Bytes& file) {
    test::MemorySource src(file);
    native->reader->begin(src, test::arena(1), formats::ReadParams{});
    while (native->reader->step(4096) == formats::Progress::Running) {
    }
    return native->reader->progress();
  };
  Tolerant t;
  NEED(read(session_file()) == formats::Progress::Done);
  CHECK(native->reader->report().sanitised == 0u);
  std::vector<RawChunk> out = t.cs;
  for (RawChunk& c : out) {
    if (c.id == kChunkNote) {
      c.payload[2] = 99;
      c.payload[6] = 99;
    }
  }
  NEED(read(build(out)) == formats::Progress::Done);
  CHECK(native->reader->report().sanitised == 2u);
  CHECK(native->reader->report().error == formats::FormatError::None);
  // the writer's report has none
  test::VectorSink sink;
  native->writer->begin(proj(1), sink, formats::WriteParams{});
  while (native->writer->step(4096) == formats::Progress::Running) {
  }
  CHECK(native->writer->progress() == formats::Progress::Done);
  CHECK(native->writer->report().sanitised == 0u);
}

TEST_CASE("persist2: the shape of a chunk is the only error: a wrong length or count is Malformed, a newer major is refused, and the project is left valid and empty") {
  Tolerant t;
  Report rep;
  const auto refused = [&](const char* what, std::uint32_t id, auto fn, Error expect = Error::Malformed) {
    INFO(what);
    std::vector<RawChunk> out = t.cs;
    for (RawChunk& c : out) {
      if (c.id == id) {
        fn(c.payload);
      }
    }
    CHECK(test::load(test::arena(1), build(out), &rep) == Progress::Failed);
    CHECK(rep.error == expect);
    CHECK((proj(1).state_present() & kAllSession) == 0);  // a failed load leaves a valid empty project
    check_defaults(proj(1));
    CHECK(proj(1).check_invariants());
  };
  refused("NOTE a byte short", kChunkNote, [](Bytes& p) { p.pop_back(); });
  refused("NOTE a byte long", kChunkNote, [](Bytes& p) { p.push_back(0); });
  refused("NOTE version 0", kChunkNote, [](Bytes& p) { p[0] = 0; p[1] = 0; });
  refused("MUTE no tracks", kChunkMute, [](Bytes& p) { p[2] = 0; });
  refused("MUTE 17 tracks", kChunkMute, [](Bytes& p) { p[2] = 17; p.push_back(0); });
  refused("MUTE a count above its length", kChunkMute, [](Bytes& p) { p.pop_back(); });
  refused("MUTE a count below its length", kChunkMute, [](Bytes& p) { p[2] = 15; });
  refused("MUTE a byte long", kChunkMute, [](Bytes& p) { p.push_back(0); });
  refused("MUTE version 0", kChunkMute, [](Bytes& p) { p[0] = 0; p[1] = 0; });
  refused("SCEN 2 scenes", kChunkScen, [](Bytes& p) { p[3] = 2; });
  refused("SCEN 4 scenes", kChunkScen, [](Bytes& p) { p[3] = 4; });
  refused("SCEN no scenes", kChunkScen, [](Bytes& p) { p[3] = 0; });
  refused("SCEN a byte short", kChunkScen, [](Bytes& p) { p.pop_back(); });
  refused("SCEN a byte long", kChunkScen, [](Bytes& p) { p.push_back(0); });
  refused("SCEN cut after its count", kChunkScen, [](Bytes& p) { p.resize(4); });
  refused("SCEN version 0", kChunkScen, [](Bytes& p) { p[0] = 0; p[1] = 0; });
  refused("SNFX no rows", kChunkSnfx, [](Bytes& p) { p.resize(3); p[2] = 0; });
  refused("SNFX 33 rows", kChunkSnfx, [](Bytes& p) { p[2] = 33; for (int i = 0; i < 42; ++i) p.push_back(0); });
  refused("SNFX a count above its length", kChunkSnfx, [](Bytes& p) { p[2] = 13; });
  refused("SNFX a count below its length", kChunkSnfx, [](Bytes& p) { p[2] = 11; });
  refused("SNFX a byte long", kChunkSnfx, [](Bytes& p) { p.push_back(0); });
  refused("SNFX version 0", kChunkSnfx, [](Bytes& p) { p[0] = 0; p[1] = 0; });
  // a newer major is refused whatever the length (the reader cannot know the layout)
  for (std::uint32_t id : {kChunkNote, kChunkMute, kChunkScen, kChunkSnfx}) {
    refused("major 2, as long as 1.0", id, [](Bytes& p) { p[0] = 2; }, Error::NewerMajor);
    refused("major 2, 3 bytes", id, [](Bytes& p) { p.resize(3); p[0] = 2; p[1] = 0; }, Error::NewerMajor);
    refused("major 2, 700 bytes", id, [](Bytes& p) { p[0] = 2; p.resize(700, 0xAB); }, Error::NewerMajor);
  }
}

TEST_CASE("persist2: a newer minor only appends: it is read as far as 1.0 goes at any length, the report says so, and the next save is the 1.0 file") {
  const Bytes file = session_file();
  const std::vector<RawChunk> cs = test::split_chunks(file);
  Report rep;
  for (std::uint32_t id : {kChunkNote, kChunkMute, kChunkScen, kChunkSnfx}) {
    const bool note = id == kChunkNote;  // OS-SCALES28: NOTE is at 1.1, so its newer minor is 1.2; the first appended byte is then read as the 1.1 scale, 0xAB above 38: counted, and the 1.0 byte's scale stands
    for (std::size_t extra : {std::size_t{1}, std::size_t{2}, std::size_t{33}, std::size_t{5000}}) {
      INFO("chunk " << id << " + " << extra);
      std::vector<RawChunk> out = cs;
      for (RawChunk& c : out) {
        if (c.id == id) {
          c.payload[1] = id == kChunkScen ? 3 : (note ? 2 : 1);  // (SCEN is at 1.2 since OS-PERFMODES M3, NOTE at 1.1 since OS-SCALES28: their newer minors are 1.3 and 1.2)
          c.payload.insert(c.payload.end(), extra, 0xAB);
        }
      }
      NEED(test::load(test::arena(1), build(out), &rep) == Progress::Done);
      CHECK(rep.newer_minor);
      CHECK(rep.settings_skipped == (note ? 1u : 0u));
      CHECK(proj(1).note() == proj(2).note());
      CHECK(proj(1).mute() == proj(2).mute());
      CHECK(proj(1).scenes() == proj(2).scenes());
      CHECK(proj(1).send_fx() == proj(2).send_fx());
      Bytes back;
      NEED(test::save(proj(1), back) == Progress::Done);
      CHECK(back == file);  // the appended fields are lost, the file is the one this version writes
    }
    // a newer minor is still at least as long as 1.0 needs
    std::vector<RawChunk> out = cs;
    for (RawChunk& c : out) {
      if (c.id == id) {
        c.payload[1] = 1;
        c.payload.resize(2);
      }
    }
    CHECK(test::load(test::arena(1), build(out), &rep) == Progress::Failed);
    CHECK(rep.error == Error::Malformed);
  }
}

TEST_CASE("persist2: the order of the chunks: NOTE, MUTE, SCEN, SNFX after APPM, each once, and an unknown chunk is kept wherever it is") {
  const Bytes file = session_file();
  const std::vector<RawChunk> cs = test::split_chunks(file);
  std::size_t at = 0;
  while (cs[at].id != kChunkNote) {
    ++at;
  }
  Report rep;
  const auto refused = [&](const char* what, const std::vector<RawChunk>& edit) {
    INFO(what);
    CHECK(test::load(test::arena(1), build(edit), &rep) == Progress::Failed);
    CHECK(rep.error == Error::Malformed);
    CHECK((proj(1).state_present() & kAllSession) == 0);
    CHECK(proj(1).check_invariants());
  };
  const auto swapped = [&](std::size_t a, std::size_t b) {
    std::vector<RawChunk> out = cs;
    std::swap(out[at + a], out[at + b]);
    return out;
  };
  refused("MUTE before NOTE", swapped(0, 1));
  refused("SCEN before MUTE", swapped(1, 2));
  refused("SNFX before SCEN", swapped(2, 3));
  refused("SNFX before NOTE", swapped(0, 3));
  std::vector<RawChunk> twice = cs;
  twice.insert(twice.begin() + static_cast<std::ptrdiff_t>(at), cs[at]);
  refused("NOTE twice", twice);
  twice = cs;
  twice.insert(twice.begin() + static_cast<std::ptrdiff_t>(at + 4), cs[at + 3]);
  refused("SNFX twice", twice);
  std::vector<RawChunk> early = cs;  // NOTE before the last INST
  early.insert(early.begin() + 2, cs[at]);
  early.erase(early.begin() + static_cast<std::ptrdiff_t>(at + 1));
  refused("NOTE before the instruments", early);
  // and before the CRC: a session chunk after an unknown chunk is fine (an unknown chunk may be anywhere), the unknown ones are kept and written after the known
  std::vector<RawChunk> mixed;
  for (std::size_t i = 0; i < cs.size(); ++i) {
    if (i == at + 2) {
      mixed.push_back({fourcc('Z', 'Z', 'Z', 'Z'), {'a', 'b', 'c'}});
    }
    mixed.push_back(cs[i]);
  }
  NEED(test::load(test::arena(1), build(mixed), &rep) == Progress::Done);
  CHECK(proj(1).unknown_count() == 3u);  // ZZZZ, XTRA, EMPT
  CHECK(proj(1).unknown(0)->id == fourcc('Z', 'Z', 'Z', 'Z'));
  CHECK(proj(1).unknown(1)->id == fourcc('X', 'T', 'R', 'A'));
  Bytes back;
  NEED(test::save(proj(1), back) == Progress::Done);
  std::vector<RawChunk> expect;
  for (const RawChunk& c : cs) {
    if (c.id != fourcc('X', 'T', 'R', 'A') && c.id != fourcc('E', 'M', 'P', 'T')) {
      expect.push_back(c);
    }
  }
  expect.push_back({fourcc('Z', 'Z', 'Z', 'Z'), {'a', 'b', 'c'}});
  expect.push_back(*std::find_if(cs.begin(), cs.end(), [](const RawChunk& c) { return c.id == fourcc('X', 'T', 'R', 'A'); }));
  expect.push_back(*std::find_if(cs.begin(), cs.end(), [](const RawChunk& c) { return c.id == fourcc('E', 'M', 'P', 'T'); }));
  CHECK(back == build(expect));
}

TEST_CASE("persist2: unknown chunks are kept next to the session chunks, a chunk of a later version too, and the four are never mistaken for unknown ones") {
  // A chunk this version does not know (a later one: LIVE) in a file that has all four: it is kept in the pool and written back after them, and the four are decoded, not kept.
  const Bytes file = session_file();
  std::vector<RawChunk> cs = test::split_chunks(file);
  std::size_t at = 0;
  while (cs[at].id != kChunkSnfx) {
    ++at;
  }
  cs.insert(cs.begin() + static_cast<std::ptrdiff_t>(at + 1), RawChunk{fourcc('L', 'I', 'V', 'E'), Bytes{0x01, 0x00, 1, 2, 3, 4, 5}});
  Report rep;
  NEED(test::load(test::arena(1), build(cs), &rep) == Progress::Done);
  CHECK(rep.unknown_kept == 3u);  // LIVE, XTRA, EMPT: not NOTE, MUTE, SCEN or SNFX
  for (std::uint32_t i = 0; i < proj(1).unknown_count(); ++i) {
    CHECK_FALSE(is_session(proj(1).unknown(i)->id));
  }
  CHECK((proj(1).state_present() & kAllSession) == kAllSession);
  Bytes back;
  NEED(test::save(proj(1), back) == Progress::Done);
  const std::vector<RawChunk> out = test::split_chunks(back);
  std::vector<std::uint32_t> ids;
  for (const RawChunk& c : out) {
    ids.push_back(c.id);
  }
  CHECK(ids[ids.size() - 3] == fourcc('L', 'I', 'V', 'E'));  // the unknown chunks keep the order they were read in, after every known one
  CHECK(ids[ids.size() - 2] == fourcc('X', 'T', 'R', 'A'));
  CHECK(find(out, fourcc('L', 'I', 'V', 'E'))->payload == Bytes({0x01, 0x00, 1, 2, 3, 4, 5}));
  // and an unknown chunk that fills the pool does not displace them: they are not in it
  Project& p = proj(0);
  test::fill_persist2(p);
  CHECK(p.unknown_dropped_chunks() == 0u);
}

TEST_CASE("persist2: truncation at every offset and one flipped bit in every byte of the session chunks never crash: Done with a valid project, or a typed error with an empty one") {
  const Bytes file = session_file();
  const std::vector<RawChunk> cs = test::split_chunks(file);
  Report rep;
  // truncated files: every offset
  for (std::size_t cut = 0; cut < file.size(); cut += 1) {
    const Bytes cutfile(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(cut));
    const Progress p = test::load(test::arena(1), cutfile, &rep);
    NEED(p == Progress::Failed);
    NEED(rep.error == Error::Truncated || rep.error == Error::BadMagic || rep.error == Error::Malformed);
    NEED(proj(1).check_invariants());
    NEED((proj(1).state_present() & kAllSession) == 0);
  }
  // a flipped bit in each byte of the four chunks, the CRC made right so that the parsers behind it are reached
  std::size_t flips = 0;
  std::size_t ok = 0;
  for (std::size_t k = 0; k < cs.size(); ++k) {
    if (!is_session(cs[k].id)) {
      continue;
    }
    for (std::size_t i = 0; i < cs[k].payload.size(); ++i) {
      for (int bit = 0; bit < 8; ++bit) {
        std::vector<RawChunk> out = cs;
        out[k].payload[i] = static_cast<std::uint8_t>(out[k].payload[i] ^ (1u << bit));
        const Progress p = test::load(test::arena(1), build(out), &rep);
        ++flips;
        NEED(proj(1).check_invariants());
        if (p == Progress::Done) {
          ++ok;
          // whatever was read is valid, and writes back to a file that loads to the same
          Bytes again;
          NEED(test::save(proj(1), again) == Progress::Done);
          NEED(test::load(test::arena(0), again, &rep) == Progress::Done);
          NEED(proj(0).note() == proj(1).note());
          NEED(proj(0).mute() == proj(1).mute());
          NEED(proj(0).scenes() == proj(1).scenes());
          NEED(proj(0).send_fx() == proj(1).send_fx());
          NEED(rep.settings_skipped == 0u);  // what was taken is clean
        } else {
          NEED(rep.error == Error::Malformed || rep.error == Error::NewerMajor);
        }
      }
    }
  }
  CHECK(flips == (7u + 19u + 295u + 27u) * 8u);
  CHECK(ok > flips / 2);  // most single-bit errors in a value are not errors at all: the value is its default
}

TEST_CASE("persist2: a random mutation fuzz of the session chunks (a few bytes of any of the four rewritten): never a crash, never an invalid project") {
  const Bytes file = session_file();
  const std::vector<RawChunk> cs = test::split_chunks(file);
  test::Rng r(1013);
  Report rep;
  for (int round = 0; round < 3000; ++round) {
    std::vector<RawChunk> out = cs;
    for (int n = 1 + static_cast<int>(r.next() % 6); n > 0; --n) {
      RawChunk& c = out[r.next() % out.size()];
      if (is_session(c.id) && !c.payload.empty()) {
        c.payload[r.next() % c.payload.size()] = static_cast<std::uint8_t>(r.next());
      }
    }
    if (round % 7 == 0) {  // sometimes a chunk of another length
      RawChunk& c = out[r.next() % out.size()];
      if (is_session(c.id)) {
        if ((r.next() & 1u) != 0u && c.payload.size() > 2) {
          c.payload.resize(2 + r.next() % (c.payload.size() - 2));
        } else {
          c.payload.insert(c.payload.end(), 1 + r.next() % 40, static_cast<std::uint8_t>(r.next()));
        }
      }
    }
    const Progress p = test::load(test::arena(1), build(out), &rep);
    NEED(proj(1).check_invariants());
    NEED(p == Progress::Done || rep.error != Error::None);
    if (p == Progress::Done) {
      Bytes again;
      NEED(test::save(proj(1), again) == Progress::Done);
      NEED(test::load(test::arena(0), again, &rep) == Progress::Done);
      NEED(proj(0).scenes() == proj(1).scenes());
    }
  }
}
