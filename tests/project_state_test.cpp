// SPDX-License-Identifier: GPL-3.0-or-later
// OS-004d: the project-level state that used to be runtime only (model/project_state.h): the model's rules, the Editor's wrappers, and the native format's
// chunks MIDI, USBS, MIXR, PERF and APPM and INST 1.2 (the playback engine): round trips, the exact bytes (written here from the description of
// docs/formats/o3-project.md, not by the writer), the older file read with defaults, the strict reader, and the unknown-pool arithmetic that keeps older builds safe.
#include <doctest/doctest.h>

#include "support/iff.h"
#include "support/memory_io.h"
#include "support/need.h"
#include "support/project_fixture.h"

#include <o3os/formats/native.h>
#include <o3os/model/editor.h>
#include <o3os/model/project.h>

#include <cstdint>
#include <cstring>
#include <vector>

using namespace o3os;
using namespace o3os::formats::native;
using namespace o3os::model;
using o3os::test::RawChunk;
using Bytes = std::vector<std::uint8_t>;

namespace {

Project& proj(int i = 0) { return test::arena(i).project; }

std::uint32_t float_bits(float f) {
  std::uint32_t b;
  std::memcpy(&b, &f, sizeof b);
  return b;
}

const RawChunk* find(const std::vector<RawChunk>& cs, std::uint32_t id, int nth = 0) {
  for (const RawChunk& c : cs) {
    if (c.id == id && nth-- == 0) {
      return &c;
    }
  }
  return nullptr;
}

void put16(Bytes& b, int v) {
  b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
  b.push_back(static_cast<std::uint8_t>(v & 0xFF));
}

// The payload of every state chunk of fill_state(), from the format's description.
Bytes expected_midi() {
  Bytes b = {0x01, 0x00, 3, 2, 9, 2, 74, 71, 255, 255, 255, 127};
  for (int v : {15, -250, -1000, 1000}) {
    put16(b, v);
  }
  return b;
}

Bytes expected_usb() {
  Bytes b = {0x01, 0x00, 1, 1, 8};
  for (int v : {0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020, 0x3003, 0xF000}) {
    put16(b, v);
  }
  return b;
}

Bytes expected_mix() {
  Bytes b = {0x01, 0x00, 17};
  for (int i = 0; i < 17; ++i) {
    b.push_back(i == 0 ? 7 : (i == 5 ? 0x30 : (i == 16 ? 0x1B : 0x21)));  // MAIN+USB (33) unless the fixture changed it; the input strip: MAIN+CUE+SNDB+REC
  }
  for (int v : {3, 2, 1, 64, 75}) {  // the returns' routes, OUT mode, CUE level, input level
    b.push_back(static_cast<std::uint8_t>(v));
  }
  return b;
}

Bytes expected_perf() {
  Bytes b = {0x01, 0x00, 12};
  for (int s = 0; s < 12; ++s) {
    const int fx = s == 2 ? 0 : (s == 11 ? 19 : s + 1);
    const int row = s == 0 ? 2 : (s == 4 ? 1 : 0);
    int v[3] = {0, 0, 0};
    if (s == 0) {
      v[0] = -100;
      v[1] = 100;
      v[2] = 1;
    } else if (s == 4) {
      v[0] = 55;
    } else if (s == 11) {
      v[2] = 8;
    }
    b.push_back(static_cast<std::uint8_t>(fx));
    b.push_back(static_cast<std::uint8_t>(row));
    for (int k = 0; k < 3; ++k) {
      put16(b, v[k]);
    }
  }
  return b;
}

Bytes expected_apps() {
  Bytes b = {0x01, 0x00, 2};
  const auto entry = [&b](const char* name, int major, int minor, int count) {
    b.push_back(static_cast<std::uint8_t>(std::strlen(name)));
    for (const char* p = name; *p != '\0'; ++p) {
      b.push_back(static_cast<std::uint8_t>(*p));
    }
    b.push_back(static_cast<std::uint8_t>(major));
    b.push_back(static_cast<std::uint8_t>(minor));
    b.push_back(static_cast<std::uint8_t>(count));
    for (int i = 0; i < count; ++i) {
      const std::uint32_t bits = float_bits(static_cast<float>(i) * 0.5f - 3.0f);
      for (int sh = 24; sh >= 0; sh -= 8) {
        b.push_back(static_cast<std::uint8_t>(bits >> sh));
      }
    }
  };
  entry("pads", 1, 1, 5);
  entry("fm-tool", 1, 0, 32);
  return b;
}

Bytes state_file() {
  test::fill_state(proj(2));
  Bytes out;
  CHECK(test::save(proj(2), out) == Progress::Done);
  return out;
}

Bytes build(const std::vector<RawChunk>& cs) { return test::build_file(cs, {}); }

bool is_state(std::uint32_t id) { return id == kChunkMidi || id == kChunkUsbs || id == kChunkMixr || id == kChunkPerf || id == kChunkAppm; }

// The version of a chunk's payload (its low byte), or -1 when the file has no such chunk.
int chunk_minor(const std::vector<RawChunk>& cs, std::uint32_t id) {
  const RawChunk* const c = find(cs, id);
  return c == nullptr ? -1 : c->payload[1];
}

}  // namespace

TEST_CASE("project state: a fresh project has no part, the defaults of the runtime objects, and a default file has no state chunk") {
  Project& p = proj(0);
  p.reset();
  CHECK(p.state_present() == 0);
  CHECK(p.app_params_count() == 0);
  MixState mix;
  init_mix_state(mix);
  CHECK(p.mix() == mix);
  CHECK(mix.route[0] == 33);   // MAIN + USB: graph.cpp init_defaults
  CHECK(mix.route[16] == 3);   // the input strip: MAIN + CUE
  CHECK(mix.cue_level == 100);
  PerfState perf;
  init_perf_state(perf);
  CHECK(p.perf() == perf);
  CHECK(perf.fx[0] == 1);      // the first twelve FX of the list, in order
  CHECK(perf.fx[11] == 12);
  CHECK(p.check_invariants());
  Bytes out;
  NEED(test::save(p, out) == Progress::Done);
  CHECK(out.size() == 186u);  // the default project of docs/formats/o3-project.md section 4, unchanged
  CHECK(test::split_chunks(out).size() == 4u);  // PHDR TRAK MODR SONG
  // the model's defaults of the stem map are the factory slots of the default profile
  UsbState usb;
  init_usb_state(usb);
  CHECK(usb.slots == kUsbProfileChannels[usb.profile]);
  CHECK(validate_usb_state(usb) == nullptr);
  MidiState midi;
  init_midi_state(midi);
  CHECK(validate_midi_state(midi) == nullptr);
}

TEST_CASE("project state: set_* checks the value, marks the part present, and changes the generation only when something changed") {
  Project& p = proj(0);
  p.reset();
  MidiState m;
  init_midi_state(m);
  std::uint32_t gen = p.generation();
  CHECK(p.set_midi(m) == Status::Ok);  // the first set marks it present even with the defaults
  CHECK((p.state_present() & Project::kStateMidi) != 0);
  CHECK(p.generation() != gen);
  gen = p.generation();
  CHECK(p.set_midi(m) == Status::Ok);  // the same again: nothing
  CHECK(p.generation() == gen);
  m.out_ports = 3;
  CHECK(p.set_midi(m) == Status::Ok);
  CHECK(p.generation() != gen);
  CHECK(p.midi().out_ports == 3);
  gen = p.generation();
  MidiState bad = m;
  bad.out_ports = 4;
  CHECK(p.set_midi(bad) == Status::BadValue);
  bad = m;
  bad.notes_channel = 16;
  CHECK(p.set_midi(bad) == Status::BadValue);
  bad = m;
  bad.follow = 3;
  CHECK(p.set_midi(bad) == Status::BadValue);
  bad = m;
  bad.cc_number[2] = 128;
  CHECK(p.set_midi(bad) == Status::BadValue);
  bad = m;
  bad.out_offset_dms[1] = 1001;
  CHECK(p.set_midi(bad) == Status::BadValue);
  bad = m;
  bad.in_offset_dms[0] = -1001;
  CHECK(p.set_midi(bad) == Status::BadValue);
  CHECK(p.generation() == gen);  // a rejected value changes nothing
  CHECK(p.midi() == m);

  UsbState u;
  init_usb_state(u);
  CHECK(p.set_usb(u) == Status::Ok);
  u.mode = 1;
  u.profile = 3;  // P11
  u.slots = 11;
  u.mask[10] = 0x8000;
  CHECK(p.set_usb(u) == Status::Ok);
  UsbState bad_usb = u;
  bad_usb.slots = 10;  // not P11's
  CHECK(p.set_usb(bad_usb) == Status::BadValue);
  bad_usb = u;
  bad_usb.profile = 4;  // P4S is not stored
  CHECK(p.set_usb(bad_usb) == Status::BadValue);
  bad_usb = u;
  bad_usb.mode = 2;
  CHECK(p.set_usb(bad_usb) == Status::BadValue);
  UsbState p2;
  std::memset(&p2, 0, sizeof p2);
  p2.profile = 0;
  p2.slots = 2;
  p2.mask[2] = 1;  // a mask past the profile's slots
  CHECK(p.set_usb(p2) == Status::BadValue);
  p2.mask[2] = 0;
  CHECK(p.set_usb(p2) == Status::Ok);

  MixState x;
  init_mix_state(x);
  x.route[16] = 3 | 32;  // the input strip has no USB column
  CHECK(p.set_mix(x) == Status::BadValue);
  x.route[16] = 3;
  x.route[1] = 64;
  CHECK(p.set_mix(x) == Status::BadValue);
  x.route[1] = 63;
  x.ret_route[0] = 4;
  CHECK(p.set_mix(x) == Status::BadValue);
  x.ret_route[0] = 3;
  x.out_mode = 2;
  CHECK(p.set_mix(x) == Status::BadValue);
  x.out_mode = 1;
  x.cue_level = 101;
  CHECK(p.set_mix(x) == Status::BadValue);
  x.cue_level = 0;
  x.in_level = 100;
  CHECK(p.set_mix(x) == Status::Ok);
  CHECK(p.mix() == x);

  PerfState s;
  init_perf_state(s);
  s.fx[0] = 22;  // (OS-PERFFX P1: 20 and 21 are BITDEPTH and OVERDRV)
  CHECK(p.set_perf(s) == Status::BadValue);
  s.fx[0] = 21;
  s.row[3] = 4;
  CHECK(p.set_perf(s) == Status::BadValue);
  s.row[3] = 3;
  s.value[5][2] = 101;
  CHECK(p.set_perf(s) == Status::BadValue);
  s.value[5][2] = -100;
  CHECK(p.set_perf(s) == Status::Ok);
  CHECK(p.state_present() == (Project::kStateMidi | Project::kStateUsb | Project::kStateMix | Project::kStatePerf));
  CHECK(p.check_invariants());
  p.reset();  // a reset forgets all of it
  CHECK(p.state_present() == 0);
  MixState def;
  init_mix_state(def);
  CHECK(p.mix() == def);
}

TEST_CASE("project state: the apps' parameters: replaced by name, newest last, the oldest dropped at six, bad input refused") {
  Project& p = proj(0);
  p.reset();
  float v[kMaxAppParams];
  for (std::uint32_t i = 0; i < kMaxAppParams; ++i) {
    v[i] = static_cast<float>(i);
  }
  CHECK(p.set_app_params("a", 1, 1, 0, v, 3) == Status::Ok);
  CHECK(p.set_app_params("b", 1, 1, 0, v, 1) == Status::Ok);
  CHECK(p.app_params_count() == 2);
  std::uint32_t gen = p.generation();
  CHECK(p.set_app_params("b", 1, 1, 0, v, 1) == Status::Ok);  // the newest says the same: nothing
  CHECK(p.generation() == gen);
  CHECK(p.set_app_params("a", 1, 1, 0, v, 3) == Status::Ok);  // an older one the same: replaced, moved to the end (the order is the change)
  CHECK(p.generation() != gen);
  CHECK(std::strcmp(p.app_params(0)->name, "b") == 0);
  CHECK(std::strcmp(p.app_params(1)->name, "a") == 0);
  v[0] = 9.5f;
  CHECK(p.set_app_params("a", 1, 1, 1, v, 4) == Status::Ok);  // more values, a newer minor
  CHECK(p.find_app_params("a", 1)->count == 4);
  CHECK(p.find_app_params("a", 1)->abi_minor == 1);
  CHECK(p.find_app_params("a", 1)->value[0] == 9.5f);
  CHECK(p.find_app_params("a", 1)->value[4] == 0.0f);  // past the count: zero
  CHECK(p.find_app_params("zz", 2) == nullptr);
  for (const char* name : {"c", "d", "e", "f", "g", "h", "i", "j", "k", "l"}) {  // (OS-APPUX S3: twelve apps are remembered now, not six)
    CHECK(p.set_app_params(name, 1, 1, 0, v, 1) == Status::Ok);
  }
  CHECK(p.app_params_count() == kMaxAppParamSets);
  CHECK(p.set_app_params("x", 1, 1, 0, v, 2) == Status::Ok);  // full: "b", the oldest, goes
  CHECK(p.app_params_count() == kMaxAppParamSets);
  CHECK(p.find_app_params("b", 1) == nullptr);
  CHECK(p.find_app_params("x", 1) != nullptr);
  CHECK(std::strcmp(p.app_params(kMaxAppParamSets - 1)->name, "x") == 0);
  CHECK(p.check_invariants());

  gen = p.generation();
  CHECK(p.set_app_params("", 0, 1, 0, v, 1) == Status::BadValue);
  CHECK(p.set_app_params("a/b", 3, 1, 0, v, 1) == Status::BadValue);
  CHECK(p.set_app_params("..", 2, 1, 0, v, 1) == Status::BadValue);
  CHECK(p.set_app_params("a", 1, 1, 0, v, 0) == Status::BadValue);
  CHECK(p.set_app_params("a", 1, 1, 0, v, kMaxAppParams + 1) == Status::BadValue);
  CHECK(p.set_app_params("a", 1, 256, 0, v, 1) == Status::BadValue);
  const char longest[33] = "0123456789abcdef0123456789abcdef";  // 32 characters: one too many
  CHECK(p.set_app_params(longest, 32, 1, 0, v, 1) == Status::BadValue);
  CHECK(p.set_app_params(longest, 31, 1, 0, v, 1) == Status::Ok);
  gen = p.generation();
  const std::uint32_t nan_bits = 0x7FC00000u;
  float nan_value;
  std::memcpy(&nan_value, &nan_bits, sizeof nan_value);
  v[2] = nan_value;
  CHECK(p.set_app_params("h", 1, 1, 0, v, 3) == Status::BadValue);
  CHECK(p.generation() == gen);
  p.clear_app_params();
  CHECK(p.app_params_count() == 0);
}

TEST_CASE("project state: the Editor's wrappers keep the undo history, the Project's own setters end it") {
  alignas(8) static std::uint8_t ring[64 * 1024];
  Project& p = proj(0);
  p.reset();
  UndoJournal journal;
  NEED(journal.begin(ring, sizeof ring));
  Editor ed(p, journal);
  CHECK(ed.create_pattern(1, 16) == Status::Ok);
  CHECK(ed.can_undo());
  MixState x;
  init_mix_state(x);
  x.cue_level = 40;
  float v[2] = {1.0f, 2.0f};
  CHECK(ed.set_mix_state(x) == Status::Ok);
  CHECK(ed.set_app_params("tool", 4, 1, 0, v, 2) == Status::Ok);
  PerfState s;
  init_perf_state(s);
  s.row[1] = 1;
  CHECK(ed.set_perf_state(s) == Status::Ok);
  MidiState m;
  init_midi_state(m);
  CHECK(ed.set_midi_state(m) == Status::Ok);
  UsbState u;
  init_usb_state(u);
  CHECK(ed.set_usb_state(u) == Status::Ok);
  CHECK(ed.set_mix_state(x) == Status::Ok);  // unchanged: nothing
  CHECK(p.mix().cue_level == 40);
  CHECK(ed.can_undo());  // the history is still the history of the pattern
  CHECK(ed.undo() == UndoResult::Ok);
  CHECK(p.pattern(1) == nullptr);
  CHECK(p.mix().cue_level == 40);  // the state is not part of any undo unit
  CHECK(ed.redo() == UndoResult::Ok);
  CHECK(ed.can_undo());
  // A change made behind the editor's back (the Project's own setter) ends the history: that is what the wrappers are for.
  x.cue_level = 41;
  CHECK(p.set_mix(x) == Status::Ok);
  CHECK_FALSE(ed.can_undo());
  // a history that was already stale stays so (the wrapper does not hide it)
  x.cue_level = 42;
  CHECK(ed.set_mix_state(x) == Status::Ok);
  CHECK_FALSE(ed.can_undo());
  CHECK(ed.set_mix_state(x) == Status::Ok);
  MixState bad = x;
  bad.out_mode = 9;
  CHECK(ed.set_mix_state(bad) == Status::BadValue);
}

// OS-PERFFX P1, decision D5 (option A, maintainer 2026-10-19): the FX ids 20 (BITDEPTH, 0..15) and 21 (OVERDRV, 0..100) fit the byte of the PERF chunk that was always there, so the layout and the
// length do not change, but the VERSION does: the writer writes 1.1 only when a slot holds one of them and 1.0, byte for byte, for every other bank (the pattern of INST 1.2). The reader loads both: 20 and 21
// are valid only in 1.1. An older build (which knows PERF at 1.0 and reads an id past 19 as none, with its row and values reset: the 1.0 reading below is exactly its rule) meets a 1.1 chunk as a newer
// minor, reads it whole, empties those slots and writes 1.0 on its next save, which docs/formats/o3-project.md says.
TEST_CASE("project state P1: PERF is 1.0 byte for byte while the ids 20 and 21 are unused, 1.1 when a slot holds one, and both versions load") {
  // a bank without them is the chunk it always was (the fixture holds ids up to 19)
  {
    const Bytes plain = state_file();
    const std::vector<RawChunk> cs = test::split_chunks(plain);
    const RawChunk* const perf = find(cs, kChunkPerf);
    NEED(perf != nullptr);
    CHECK(perf->payload == expected_perf());  // (0x0100 in front)
    CHECK(perf->payload[0] == 0x01);
    CHECK(perf->payload[1] == 0x00);
  }
  test::fill_state(proj(2));
  PerfState s = proj(2).perf();
  s.fx[2] = 20;   // BITDEPTH
  s.row[2] = 3;
  s.value[2][0] = 1;
  s.value[2][1] = 8;
  s.value[2][2] = 15;
  s.fx[6] = 21;   // OVERDRV
  s.row[6] = 1;
  s.value[6][0] = 100;
  s.value[6][1] = 50;
  s.value[6][2] = 0;
  CHECK(proj(2).set_perf(s) == Status::Ok);
  Bytes file;
  NEED(test::save(proj(2), file) == Progress::Done);
  const std::vector<RawChunk> cs = test::split_chunks(file);
  const RawChunk* const perf = find(cs, kChunkPerf);
  NEED(perf != nullptr);
  CHECK(perf->payload.size() == 99u);           // the same length as 1.0
  CHECK(perf->payload[0] == 0x01);              // version 1.1
  CHECK(perf->payload[1] == 0x01);
  CHECK(perf->payload[2] == 12);                // the slot count: unchanged
  CHECK(perf->payload[3 + 2 * 8] == 20);        // slot 2: the id, the row, then the three values
  CHECK(perf->payload[3 + 2 * 8 + 1] == 3);
  CHECK(perf->payload[3 + 2 * 8 + 2 + 2 * 2 + 1] == 15);
  CHECK(perf->payload[3 + 6 * 8] == 21);
  CHECK(perf->payload[3 + 6 * 8 + 3] == 100);
  Report rep;
  NEED(test::load(test::arena(1), file, &rep) == Progress::Done);  // a 1.1 file loads
  CHECK_FALSE(rep.newer_minor);                 // this build knows 1.1
  CHECK(rep.settings_skipped == 0u);            // nothing was reset
  CHECK(proj(1).perf() == s);
  CHECK(proj(1).check_invariants());
  Bytes again;
  NEED(test::save(proj(1), again) == Progress::Done);
  CHECK(again == file);                         // 1.1 round-trips byte for byte
  // the ids end up unused again: the chunk is 1.0 again, whatever version it was read as
  s.fx[2] = 3;
  s.fx[6] = 7;
  CHECK(proj(1).set_perf(s) == Status::Ok);
  Bytes back;
  NEED(test::save(proj(1), back) == Progress::Done);
  const std::vector<RawChunk> back_cs = test::split_chunks(back);
  NEED(find(back_cs, kChunkPerf) != nullptr);
  CHECK(find(back_cs, kChunkPerf)->payload[1] == 0x00);
  // a 1.1 chunk that holds neither id loads, and is written back as 1.0
  {
    std::vector<RawChunk> t = test::split_chunks(back);
    for (RawChunk& c : t) {
      if (c.id == kChunkPerf) {
        c.payload[1] = 0x01;
      }
    }
    NEED(test::load(test::arena(1), build(t), &rep) == Progress::Done);
    CHECK_FALSE(rep.newer_minor);
    CHECK(proj(1).perf() == s);
    Bytes saved;
    NEED(test::save(proj(1), saved) == Progress::Done);
    CHECK(saved == back);
  }
  // 20 and 21 are valid only in 1.1: in a 1.0 chunk they read as none, row and values reset (an older build's reading of every 1.1 file); the other slots are as the file said
  {
    std::vector<RawChunk> t = test::split_chunks(file);
    for (RawChunk& c : t) {
      if (c.id == kChunkPerf) {
        c.payload[1] = 0x00;
      }
    }
    NEED(test::load(test::arena(1), build(t), &rep) == Progress::Done);
    const PerfState& pf = proj(1).perf();
    for (const unsigned i : {2u, 6u}) {
      CHECK(pf.fx[i] == 0);
      CHECK(pf.row[i] == 0);
      CHECK(pf.value[i][0] == 0);
      CHECK(pf.value[i][1] == 0);
      CHECK(pf.value[i][2] == 0);
    }
    CHECK(pf.fx[0] == 1);
    CHECK(pf.fx[4] == 5);
    CHECK(pf.row[4] == 1);
    CHECK(proj(1).check_invariants());
    Bytes saved;
    NEED(test::save(proj(1), saved) == Progress::Done);
    const std::vector<RawChunk> saved_cs = test::split_chunks(saved);
    NEED(find(saved_cs, kChunkPerf) != nullptr);
    CHECK(find(saved_cs, kChunkPerf)->payload[1] == 0x00);  // and it writes 1.0
  }
  // in 1.1 an id above 21 is still none, and 20 and 21 stay
  {
    std::vector<RawChunk> t = test::split_chunks(file);
    for (RawChunk& c : t) {
      if (c.id == kChunkPerf) {
        c.payload[3] = 22;  // slot 0
      }
    }
    NEED(test::load(test::arena(1), build(t), &rep) == Progress::Done);
    CHECK(proj(1).perf().fx[0] == 0);
    CHECK(proj(1).perf().row[0] == 0);
    CHECK(proj(1).perf().fx[2] == 20);
    CHECK(proj(1).perf().fx[6] == 21);
  }
  // a value past either FX's own range still fits the chunk's -100..100 and is the bank's to hold (PerformBank::load_slots clamps it); 101 is refused as it always was
  s.fx[2] = 20;
  s.value[2][2] = 100;
  CHECK(proj(2).set_perf(s) == Status::Ok);
  s.value[2][2] = 101;
  CHECK(proj(2).set_perf(s) == Status::BadValue);
}

// OS-PERFFX P1 (decision D5: PERF and SCEN are 1.1 only when a slot holds the Perform FX 20 or 21) and OS-APPUX S3 (APPM is 1.1 only when more than six apps are stored) are two independent version gates of
// the same file. Each feature alone bumps only its own chunks, a project with neither is the file it always was, and a project with both writes all three as 1.1 and reads them back.
TEST_CASE("project state P1 + APPM 1.1: the version gates are independent, each feature alone bumps only its own chunks, both together round-trip") {
  Bytes files[4];  // bit 0: Perform FX 20 and 21 in the slots and a stored scene; bit 1: seven apps (more than 1.0 held)
  for (unsigned mask = 0; mask < 4; ++mask) {
    const bool fx = (mask & 1u) != 0;
    const bool apps = (mask & 2u) != 0;
    INFO("FX 20/21: " << fx << ", seven apps: " << apps);
    Project& p = proj(2);
    test::fill_state(p);  // two apps, FX up to 19, no scene
    ScenesState sc;
    init_scenes_state(sc);
    sc.scene[0].stored = 1;
    init_perf_state(sc.scene[0].slots);
    sc.scene[0].slots.fx[11] = 19;
    sc.scene[0].slots.value[11][1] = 8;
    sc.scene[2].stored = 1;
    init_perf_state(sc.scene[2].slots);
    if (fx) {
      PerfState s = p.perf();
      s.fx[2] = 20;
      s.row[2] = 3;
      s.value[2][1] = 8;
      s.fx[6] = 21;
      s.value[6][0] = 100;
      CHECK(p.set_perf(s) == Status::Ok);
      sc.scene[0].slots.fx[1] = 21;  // a stored scene alone holds the id in SCEN too
      sc.scene[0].slots.value[1][0] = 50;
    }
    CHECK(p.set_scenes(sc) == Status::Ok);
    if (apps) {
      float v[2] = {1.5f, -0.25f};
      for (const char* name : {"app3", "app4", "app5", "app6", "app7"}) {
        CHECK(p.set_app_params(name, std::strlen(name), 1, 0, v, 2) == Status::Ok);
      }
    }
    CHECK(p.app_params_count() == (apps ? 7u : 2u));
    NEED(test::save(p, files[mask]) == Progress::Done);
    // every combination is read, loses nothing and writes the same bytes again (p is this combination's project)
    Report rep;
    NEED(test::load(test::arena(1), files[mask], &rep) == Progress::Done);
    CHECK_FALSE(rep.newer_minor);
    CHECK(proj(1).check_invariants());
    CHECK(proj(1).perf() == p.perf());
    CHECK(proj(1).scenes() == p.scenes());
    NEED(proj(1).app_params_count() == (apps ? 7u : 2u));
    for (std::uint32_t i = 0; i < proj(1).app_params_count(); ++i) {
      CHECK(std::memcmp(proj(1).app_params(i), p.app_params(i), sizeof(AppParams)) == 0);
    }
    Bytes again;
    NEED(test::save(proj(1), again) == Progress::Done);
    CHECK(again == files[mask]);
  }
  const std::vector<RawChunk> base = test::split_chunks(files[0]);
  // a project that uses neither feature is 1.0 everywhere the gates reach (mask 0, the file it always was); a chunk whose feature is not in use is the same bytes as there, whatever the other feature
  for (unsigned mask = 0; mask < 4; ++mask) {
    const bool fx = (mask & 1u) != 0;
    const bool apps = (mask & 2u) != 0;
    INFO("mask " << mask);
    const std::vector<RawChunk> cs = test::split_chunks(files[mask]);
    CHECK(chunk_minor(cs, kChunkPerf) == (fx ? 1 : 0));
    CHECK(chunk_minor(cs, kChunkScen) == (fx ? 1 : 0));
    CHECK(chunk_minor(cs, kChunkAppm) == (apps ? 1 : 0));
    for (const std::uint32_t id : {kChunkMidi, kChunkUsbs, kChunkMixr}) {
      CHECK(chunk_minor(cs, id) == 0);
    }
    NEED(cs.size() == base.size());
    for (std::size_t i = 0; i < cs.size(); ++i) {
      INFO("chunk " << cs[i].id);
      CHECK(cs[i].id == base[i].id);
      const bool gated = cs[i].id == kChunkPerf || cs[i].id == kChunkScen || cs[i].id == kChunkAppm || cs[i].id == kChunkCrc;
      if (!gated) {
        CHECK(cs[i].payload == base[i].payload);  // nothing else moved
      }
      if (cs[i].id == kChunkAppm && !apps) {
        CHECK(cs[i].payload == base[i].payload);  // the APPM of a project with six apps or fewer is the bytes it was, whatever the Perform FX
      }
      if ((cs[i].id == kChunkPerf || cs[i].id == kChunkScen) && !fx) {
        CHECK(cs[i].payload == base[i].payload);  // and PERF and SCEN whatever the apps
      }
    }
    CHECK(find(cs, kChunkPerf)->payload.size() == 99u);   // the lengths never changed
    CHECK(find(cs, kChunkScen)->payload.size() == 295u);
  }
  // both features in one file; each chunk is read by its own version only: PERF and SCEN as 1.0 lose the ids 20 and 21 (an older build's reading) and keep the seven apps, an APPM that says 1.0 with seven apps is refused
  {
    std::vector<RawChunk> t = test::split_chunks(files[3]);
    for (RawChunk& c : t) {
      if (c.id == kChunkPerf || c.id == kChunkScen) {
        c.payload[1] = 0x00;
      }
    }
    Report rep;
    NEED(test::load(test::arena(1), build(t), &rep) == Progress::Done);
    CHECK(proj(1).perf().fx[2] == 0);
    CHECK(proj(1).perf().fx[6] == 0);
    CHECK(proj(1).scenes().scene[0].slots.fx[1] == 0);
    CHECK(proj(1).app_params_count() == 7u);
    Bytes saved;
    NEED(test::save(proj(1), saved) == Progress::Done);
    const std::vector<RawChunk> saved_cs = test::split_chunks(saved);
    CHECK(chunk_minor(saved_cs, kChunkPerf) == 0);  // the ids are gone, so the chunks are 1.0 again
    CHECK(chunk_minor(saved_cs, kChunkScen) == 0);
    CHECK(chunk_minor(saved_cs, kChunkAppm) == 1);  // the seven apps are still there
  }
  {
    std::vector<RawChunk> t = test::split_chunks(files[3]);
    for (RawChunk& c : t) {
      if (c.id == kChunkAppm) {
        c.payload[1] = 0x00;
      }
    }
    Report rep;
    CHECK(test::load(test::arena(1), build(t), &rep) == Progress::Failed);
    CHECK(rep.error == Error::Malformed);
    CHECK(proj(1).app_params_count() == 0);
  }
}

TEST_CASE("project state: the chunks are the bytes of the description, and the file round-trips (save, load, save)") {
  const Bytes file = state_file();
  const std::vector<RawChunk> cs = test::split_chunks(file);
  // MIDI, USBS, MIXR, PERF and APPM come after the known chunks of 1.0 (here without SETS and LRNB) and before the unknown ones, in that order.
  std::vector<std::uint32_t> order;
  for (const RawChunk& c : cs) {
    order.push_back(c.id);
  }
  std::size_t at = 0;
  while (at < order.size() && order[at] != kChunkMidi) {
    ++at;
  }
  NEED(at + 5 <= order.size());
  CHECK(order[at + 1] == kChunkUsbs);
  CHECK(order[at + 2] == kChunkMixr);
  CHECK(order[at + 3] == kChunkPerf);
  CHECK(order[at + 4] == kChunkAppm);
  CHECK(order[at - 1] == kChunkInst);
  CHECK(at + 5 < order.size());  // the unknown chunks of the small fixture follow
  CHECK(find(cs, kChunkMidi)->payload == expected_midi());
  CHECK(find(cs, kChunkUsbs)->payload == expected_usb());
  CHECK(find(cs, kChunkMixr)->payload == expected_mix());
  CHECK(find(cs, kChunkPerf)->payload == expected_perf());
  CHECK(find(cs, kChunkAppm)->payload == expected_apps());
  CHECK(find(cs, kChunkMidi)->payload.size() == 20u);
  CHECK(find(cs, kChunkMixr)->payload.size() == 25u);
  CHECK(find(cs, kChunkPerf)->payload.size() == 99u);

  Report rep;
  NEED(test::load(test::arena(1), file, &rep) == Progress::Done);
  CHECK_FALSE(rep.newer_minor);
  Project& q = proj(1);
  CHECK(q.state_present() == (Project::kStateMidi | Project::kStateUsb | Project::kStateMix | Project::kStatePerf));
  CHECK(q.midi() == proj(2).midi());
  CHECK(q.usb() == proj(2).usb());
  CHECK(q.mix() == proj(2).mix());
  CHECK(q.perf() == proj(2).perf());
  NEED(q.app_params_count() == 2);
  CHECK(std::strcmp(q.app_params(0)->name, "pads") == 0);
  CHECK(q.app_params(0)->count == 5);
  CHECK(q.app_params(1)->count == 32);
  CHECK(q.app_params(1)->value[31] == 12.5f);
  CHECK(q.instrument(0)->engine == kInstEngineAmiga);
  CHECK(q.instrument(1)->engine == kInstEngineBits8);
  CHECK(q.instrument(48)->engine == kInstEngineAuto);
  CHECK(q.check_invariants());
  Bytes again;
  NEED(test::save(q, again) == Progress::Done);
  CHECK(again == file);
  Bytes small_budget;
  NEED(test::save(q, small_budget, 7) == Progress::Done);  // any step budget gives the same bytes
  CHECK(small_budget == file);

  // INST: 1.2 (one more byte, the engine) only for the instruments that chose one; the others are the 1.1 chunks they always were.
  std::vector<std::uint8_t> versions;
  for (const RawChunk& c : cs) {
    if (c.id == kChunkInst) {
      versions.push_back(c.payload[1]);
    }
  }
  CHECK(versions == std::vector<std::uint8_t>({2, 2, 1, 1}));
  const RawChunk* inst0 = find(cs, kChunkInst);
  CHECK(inst0->payload.back() == kInstEngineAmiga);
}

namespace {

// OS-USBOUTS: a USB state of `profile` (the usb::Profile value) with the factory masks of that many channels (T1.., then Master L/R), `mode` as given.
UsbState usb_of(std::uint8_t profile, std::uint8_t mode = 1) {
  UsbState u;
  std::memset(&u, 0, sizeof u);
  u.mode = mode;
  u.profile = profile;
  u.slots = kUsbProfileChannels[profile];
  for (std::uint32_t i = 0; i + 2u < u.slots; ++i) {
    u.mask[i] = static_cast<std::uint16_t>(1u << i);
  }
  u.mask[u.slots - 2u] = 0x1000;
  u.mask[u.slots - 1u] = 0x2000;
  return u;
}

const Bytes& usbs_payload(const std::vector<RawChunk>& cs) {
  static const Bytes none;
  const RawChunk* const c = find(cs, kChunkUsbs);
  return c == nullptr ? none : c->payload;
}

}  // namespace

TEST_CASE("project state OS-USBOUTS: 2, 8, 10 and 11 outputs are USBS 1.0 byte for byte, 4 and 6 are USBS 1.1 (P8 plus the count) and round-trip") {
  // 1.0: profile byte 0..3, as v1.0.x wrote it
  struct Row {
    std::uint8_t profile;
    std::uint8_t byte;
    std::uint8_t slots;
  };
  for (const Row r : {Row{0, 0, 2}, Row{1, 1, 8}, Row{2, 2, 10}, Row{3, 3, 11}}) {
    INFO("profile " << int(r.profile));
    proj(0).reset();
    const UsbState u = usb_of(r.profile);
    NEED(proj(0).set_usb(u) == Status::Ok);
    Bytes file;
    NEED(test::save(proj(0), file) == Progress::Done);
    const std::vector<RawChunk> cs = test::split_chunks(file);
    const Bytes& pl = usbs_payload(cs);
    NEED(pl.size() == 5u + 2u * r.slots);
    CHECK(pl[0] == 0x01);
    CHECK(pl[1] == 0x00);  // 1.0
    CHECK(pl[2] == 1);
    CHECK(pl[3] == r.byte);
    CHECK(pl[4] == r.slots);
    Report rep;
    NEED(test::load(test::arena(1), file, &rep) == Progress::Done);
    CHECK_FALSE(rep.newer_minor);
    CHECK(proj(1).usb() == u);
  }
  // 1.1: 22 bytes, P8 with 8 slots, the real slots first (the user's edit of slot 0 included), then P8's factory masks, then the count
  for (const std::uint8_t profile : {std::uint8_t{5}, std::uint8_t{6}}) {
    const unsigned n = kUsbProfileChannels[profile];
    INFO("channels " << n);
    proj(0).reset();
    UsbState u = usb_of(profile);
    u.mask[0] = 0x0003;  // T1 + T2 on S1
    NEED(proj(0).set_usb(u) == Status::Ok);
    Bytes file;
    NEED(test::save(proj(0), file) == Progress::Done);
    const std::vector<RawChunk> cs = test::split_chunks(file);
    const Bytes& pl = usbs_payload(cs);
    NEED(pl.size() == 22u);
    CHECK(pl[0] == 0x01);
    CHECK(pl[1] == 0x01);  // 1.1
    CHECK(pl[2] == 1);     // mode
    CHECK(pl[3] == 1);     // P8
    CHECK(pl[4] == 8);
    const std::uint16_t p8[8] = {0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020, 0x1000, 0x2000};
    for (unsigned i = 0; i < 8; ++i) {
      const std::uint16_t want = i < n ? u.mask[i] : p8[i];
      CHECK(((pl[5 + 2 * i] << 8) | pl[6 + 2 * i]) == want);
    }
    CHECK(pl[21] == n);
    Report rep;
    NEED(test::load(test::arena(1), file, &rep) == Progress::Done);
    CHECK_FALSE(rep.newer_minor);  // this build knows 1.1
    CHECK(proj(1).usb() == u);
    CHECK(proj(1).check_invariants());
    Bytes again;
    NEED(test::save(proj(1), again) == Progress::Done);
    CHECK(again == file);

    // The view of a v1.0 reader: it takes the known part (P8, 8 slots) and skips the count. Cut the count off and say 1.0: that is what it sees.
    std::vector<RawChunk> old = cs;
    for (RawChunk& c : old) {
      if (c.id == kChunkUsbs) {
        c.payload.pop_back();
        c.payload[1] = 0x00;
      }
    }
    NEED(test::load(test::arena(1), build(old), &rep) == Progress::Done);
    const UsbState& v = proj(1).usb();
    CHECK(v.profile == 1);  // P8
    CHECK(v.slots == 8);
    for (unsigned i = 0; i < 8; ++i) {
      CHECK(v.mask[i] == (i < n ? u.mask[i] : p8[i]));
    }
    CHECK(v.mode == 1);
  }
  // the other parts of a project do not change the USBS version: the default project has no USBS chunk
  proj(0).reset();
  Bytes plain;
  NEED(test::save(proj(0), plain) == Progress::Done);
  CHECK(plain.size() == 186u);
  const std::vector<RawChunk> plain_cs = test::split_chunks(plain);
  CHECK(usbs_payload(plain_cs).empty());
}

TEST_CASE("project state OS-USBOUTS: a future USBS minor (1.2) is read leniently: P2 plus extra bytes loads as P2, an unusable count falls back to the 1.0 view") {
  const auto load_with = [&](const Bytes& payload) -> Progress {
    proj(0).reset();
    Bytes file;
    if (test::save(proj(0), file) != Progress::Done) {
      return Progress::Failed;
    }
    std::vector<RawChunk> cs = test::split_chunks(file);
    RawChunk c;
    c.id = kChunkUsbs;
    c.payload = payload;
    cs.push_back(c);
    return test::load(test::arena(1), build(cs), nullptr);
  };
  // 1.2, P2 (2 slots, masks 0x0001 and 0x0002), then bytes this build does not know (the first would be a count in 1.1)
  const Bytes p2 = {0x01, 0x02, 1, 0, 2, 0x00, 0x01, 0x00, 0x02, 0x05, 0xAA, 0xBB};
  NEED(load_with(p2) == Progress::Done);
  CHECK(proj(1).usb().profile == 0);
  CHECK(proj(1).usb().slots == 2);
  CHECK(proj(1).usb().mask[0] == 0x0001);
  CHECK(proj(1).usb().mask[1] == 0x0002);
  CHECK(proj(1).check_invariants());
  // 1.2, P8 with a count this build cannot use (5): the 1.0 view, P8 with 8 slots
  Bytes p8 = {0x01, 0x02, 1, 1, 8};
  for (unsigned i = 0; i < 8; ++i) {
    p8.push_back(0x00);
    p8.push_back(static_cast<std::uint8_t>(1u << (i < 6 ? i : 0)));
  }
  p8.push_back(5);
  p8.push_back(0xCC);
  NEED(load_with(p8) == Progress::Done);
  CHECK(proj(1).usb().profile == 1);
  CHECK(proj(1).usb().slots == 8);
  // 1.2, P8 with a usable count (6): the count is used
  p8[p8.size() - 2] = 6;
  NEED(load_with(p8) == Progress::Done);
  CHECK(proj(1).usb().profile == 6);
  CHECK(proj(1).usb().slots == 6);
}

TEST_CASE("project state OS-USBOUTS: a hostile USBS 1.1 or a P4 / P6 / P4S byte in a 1.0 chunk is Malformed, never a project") {
  proj(0).reset();
  NEED(proj(0).set_usb(usb_of(5)) == Status::Ok);
  Bytes file;
  NEED(test::save(proj(0), file) == Progress::Done);
  const std::vector<RawChunk> cs = test::split_chunks(file);
  const auto refused = [&](const char* what, auto fn) {
    INFO(what);
    std::vector<RawChunk> out = cs;
    for (RawChunk& c : out) {
      if (c.id == kChunkUsbs) {
        fn(c.payload);
      }
    }
    Report rep;
    CHECK(test::load(test::arena(1), build(out), &rep) == Progress::Failed);
    CHECK(rep.error == Error::Malformed);
    CHECK(proj(1).state_present() == 0);
    CHECK(proj(1).check_invariants());
  };
  refused("count 0", [](Bytes& p) { p[21] = 0; });
  refused("count 2", [](Bytes& p) { p[21] = 2; });
  refused("count 5", [](Bytes& p) { p[21] = 5; });
  refused("count 8", [](Bytes& p) { p[21] = 8; });
  refused("count 255", [](Bytes& p) { p[21] = 255; });
  refused("profile P2 with a count", [](Bytes& p) { p[3] = 0; p[4] = 2; p.resize(p.size() - 12); p[p.size() - 1] = 4; });
  refused("profile P10 with a count", [](Bytes& p) { p[3] = 2; });
  refused("6 slots with a count", [](Bytes& p) { p[4] = 6; p.erase(p.begin() + 17, p.begin() + 21); });
  refused("the count byte missing", [](Bytes& p) { p.pop_back(); });
  refused("an unused byte after the count", [](Bytes& p) { p.push_back(0); });
  refused("a bare 1.1 header", [](Bytes& p) { p.resize(2); });
  // a 1.0 chunk holds the profile byte 0..3 only: P4S (4), and the model's P4 / P6 values (5, 6) are not stored
  for (const std::uint8_t profile : {std::uint8_t{4}, std::uint8_t{5}, std::uint8_t{6}, std::uint8_t{7}, std::uint8_t{255}}) {
    INFO("1.0 profile byte " << int(profile));
    refused("1.0 profile byte", [&](Bytes& p) {
      p.pop_back();
      p[1] = 0x00;
      p[3] = profile;
    });
  }
  // a 1.2 chunk (a newer minor) keeps what 1.1 means and may append more
  {
    std::vector<RawChunk> out = cs;
    for (RawChunk& c : out) {
      if (c.id == kChunkUsbs) {
        c.payload[1] = 0x02;
        c.payload.push_back(0x77);
      }
    }
    Report rep;
    NEED(test::load(test::arena(1), build(out), &rep) == Progress::Done);
    CHECK(rep.newer_minor);
    CHECK(proj(1).usb().profile == 5);
    CHECK(proj(1).usb().slots == 4);
  }
  // the model refuses P4S and values past P6
  UsbState u = usb_of(5);
  u.profile = 4;
  CHECK(proj(0).set_usb(u) == Status::BadValue);
  u.profile = 7;
  CHECK(proj(0).set_usb(u) == Status::BadValue);
  u = usb_of(6);
  u.slots = 4;  // not P6's
  CHECK(proj(0).set_usb(u) == Status::BadValue);
  u = usb_of(5);
  u.mask[4] = 1;  // a mask past the profile's four slots
  CHECK(proj(0).set_usb(u) == Status::BadValue);
}

TEST_CASE("project state: an older file (no state chunks, INST 1.1) reads as the defaults, and what is read is written back as it was") {
  test::fill_small(proj(0));
  Bytes old_file;
  NEED(test::save(proj(0), old_file) == Progress::Done);  // fill_small chooses no engine and no state: this is the file an older build wrote
  for (const RawChunk& c : test::split_chunks(old_file)) {
    CHECK_FALSE(is_state(c.id));
    if (c.id == kChunkInst) {
      CHECK(c.payload[1] == 1);
    }
  }
  Report rep;
  NEED(test::load(test::arena(1), old_file, &rep) == Progress::Done);
  CHECK_FALSE(rep.newer_minor);
  Project& q = proj(1);
  CHECK(q.state_present() == 0);
  CHECK(q.app_params_count() == 0);
  MixState mix;
  init_mix_state(mix);
  CHECK(q.mix() == mix);
  PerfState perf;
  init_perf_state(perf);
  CHECK(q.perf() == perf);
  for (std::uint32_t i : {0u, 48u, 50u}) {
    CHECK(q.instrument(i)->engine == kInstEngineAuto);
  }
  Bytes back;
  NEED(test::save(q, back) == Progress::Done);
  CHECK(back == old_file);

  // The state fixture minus its state chunks, with the 1.2 engine bytes taken off, is the same older file apart from what it chose.
  const Bytes fixture = state_file();
  std::vector<RawChunk> cs = test::split_chunks(fixture);
  std::vector<RawChunk> stripped;
  for (RawChunk& c : cs) {
    if (is_state(c.id)) {
      continue;
    }
    if (c.id == kChunkInst && c.payload[1] == 2) {
      c.payload[1] = 1;
      c.payload.pop_back();
    }
    stripped.push_back(c);
  }
  NEED(test::load(test::arena(1), build(stripped), &rep) == Progress::Done);
  CHECK(q.state_present() == 0);
  CHECK(q.instrument(0)->engine == kInstEngineAuto);
  CHECK(q.instrument(1)->engine == kInstEngineAuto);
  CHECK(q.instrument(1)->sample_rate == 8287u);
}

TEST_CASE("project state: INST 1.2 is exact: an engine above the table reads as automatic, a short or long chunk is refused, 1.4 is a newer minor") {
  const Bytes file = state_file();
  const std::vector<RawChunk> cs = test::split_chunks(file);
  Report rep;
  // the engine byte is the last of the chunk; 5 is not an engine of this version
  std::vector<RawChunk> edit = cs;
  for (RawChunk& c : edit) {
    if (c.id == kChunkInst && c.payload[1] == 2) {
      c.payload.back() = 5;
      break;
    }
  }
  NEED(test::load(test::arena(1), build(edit), &rep) == Progress::Done);
  CHECK_FALSE(rep.newer_minor);
  CHECK(proj(1).instrument(0)->engine == kInstEngineAuto);
  CHECK(proj(1).instrument(1)->engine == kInstEngineBits8);
  Bytes resaved;
  NEED(test::save(proj(1), resaved) == Progress::Done);
  bool first_is_11 = false;
  for (const RawChunk& c : test::split_chunks(resaved)) {
    if (c.id == kChunkInst) {
      first_is_11 = c.payload[1] == 1;  // written as 1.1 now: the engine is automatic
      break;
    }
  }
  CHECK(first_is_11);
  // 1.2 cut inside its engine byte, and 1.1 with the byte left on: unused or missing bytes
  edit = cs;
  for (RawChunk& c : edit) {
    if (c.id == kChunkInst && c.payload[1] == 2) {
      c.payload.pop_back();
      break;
    }
  }
  CHECK(test::load(test::arena(1), build(edit), &rep) == Progress::Failed);
  CHECK(rep.error == Error::Malformed);
  edit = cs;
  for (RawChunk& c : edit) {
    if (c.id == kChunkInst && c.payload[1] == 2) {
      c.payload[1] = 1;
      break;
    }
  }
  CHECK(test::load(test::arena(1), build(edit), &rep) == Progress::Failed);
  CHECK(rep.error == Error::Malformed);
  // 1.4 (1.3 is the MIDI fields since OS-INSTUI S8b, inst_midi_format_test.cpp): read as far as 1.3, here the twelve bytes of the defaults beside the engine, the appended field skipped, the report says so
  edit = cs;
  for (RawChunk& c : edit) {
    if (c.id == kChunkInst && c.payload[1] == 2) {
      c.payload[1] = 6;  // (1.4 is OS-LFOSINE's, with no field of its own; 1.5 is OS-STEREO's flag, so the next minor is 1.6)
      const std::uint8_t twelve[12] = {127, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 100};
      for (const std::uint8_t b : twelve) {
        c.payload.push_back(b);
      }
      c.payload.push_back(0);  // the stereo flag of 1.5: mono
      c.payload.push_back(0xEE);
      c.payload.push_back(0xFF);
      break;
    }
  }
  NEED(test::load(test::arena(1), build(edit), &rep) == Progress::Done);
  CHECK(rep.newer_minor);
  CHECK(proj(1).instrument(0)->engine == kInstEngineAmiga);
  // the model refuses an engine past the table (validate_instrument)
  Instrument in = *proj(1).instrument(0);
  in.engine = static_cast<std::uint8_t>(kInstEngineMax + 1);
  CHECK(validate_instrument(in, proj(1).module_count()) != nullptr);
  CHECK(proj(1).set_instrument(0, in) == Status::BadValue);
  in.engine = kInstEngineMax;
  CHECK(proj(1).set_instrument(0, in) == Status::Ok);
}

TEST_CASE("project state: a state chunk is strict: values out of range, wrong counts, wrong order and unused bytes are Malformed; a newer minor appends") {
  const Bytes file = state_file();
  const std::vector<RawChunk> cs = test::split_chunks(file);
  Report rep;
  const auto refused = [&](const char* what, const std::vector<RawChunk>& edit, Error expect = Error::Malformed) {
    INFO(what);
    CHECK(test::load(test::arena(1), build(edit), &rep) == Progress::Failed);
    CHECK(rep.error == expect);
    CHECK(proj(1).state_present() == 0);  // a failed load leaves a valid empty project
    CHECK(proj(1).app_params_count() == 0);
    CHECK(proj(1).check_invariants());
  };
  const auto with = [&](std::uint32_t id, auto fn) {
    std::vector<RawChunk> out = cs;
    for (RawChunk& c : out) {
      if (c.id == id) {
        fn(c.payload);
      }
    }
    return out;
  };
  refused("MIDI port 4", with(kChunkMidi, [](Bytes& p) { p[2] = 4; }));
  refused("MIDI note port 8", with(kChunkMidi, [](Bytes& p) { p[3] = 8; }));
  refused("MIDI channel 16", with(kChunkMidi, [](Bytes& p) { p[4] = 16; }));
  refused("MIDI follow 3", with(kChunkMidi, [](Bytes& p) { p[5] = 3; }));
  refused("MIDI CC 128", with(kChunkMidi, [](Bytes& p) { p[6] = 128; }));
  refused("MIDI offset 1001", with(kChunkMidi, [](Bytes& p) { p[12] = 0x03; p[13] = 0xE9; }));
  refused("MIDI a byte short", with(kChunkMidi, [](Bytes& p) { p.pop_back(); }));
  refused("MIDI a byte long", with(kChunkMidi, [](Bytes& p) { p.push_back(0); }));
  refused("USBS P8 with 7 slots", with(kChunkUsbs, [](Bytes& p) { p[4] = 7; p.resize(p.size() - 2); }));
  refused("USBS profile 4", with(kChunkUsbs, [](Bytes& p) { p[3] = 4; }));
  refused("USBS mode 2", with(kChunkUsbs, [](Bytes& p) { p[2] = 2; }));
  refused("MIXR 16 strips", with(kChunkMixr, [](Bytes& p) { p[2] = 16; }));
  refused("MIXR route bits", with(kChunkMixr, [](Bytes& p) { p[4] = 0x40; }));
  refused("MIXR the input strip with USB", with(kChunkMixr, [](Bytes& p) { p[19] = 0x23; }));
  refused("PERF 11 slots", with(kChunkPerf, [](Bytes& p) { p[2] = 11; }));
  refused("PERF value 101", with(kChunkPerf, [](Bytes& p) { p[5] = 0; p[6] = 101; }));
  refused("APPM no apps", with(kChunkAppm, [](Bytes& p) { p.resize(3); p[2] = 0; }));
  refused("APPM an unsafe name", with(kChunkAppm, [](Bytes& p) { p[4] = '/'; }));
  refused("APPM more apps than the project keeps", with(kChunkAppm, [](Bytes& p) { p[2] = 7; }));
  refused("APPM 33 values", with(kChunkAppm, [](Bytes& p) { p[4 + 4 + 2] = 33; }));
  refused("APPM a NaN", with(kChunkAppm, [](Bytes& p) { p[4 + 4 + 3] = 0x7F; p[4 + 4 + 4] = 0xC0; p[4 + 4 + 5] = 0; p[4 + 4 + 6] = 0; }));
  // PERF is tolerant where a later version may extend a table: an FX id above it is none (row and values reset), a row above 3 is off.
  {
    std::vector<RawChunk> t = with(kChunkPerf, [](Bytes& p) {
      p[3] = 22;       // slot 0: FX 22 (past OVERDRV, the last of OS-PERFFX P1)
      p[3 + 8 + 1] = 4;  // slot 1: row 4
    });
    NEED(test::load(test::arena(1), build(t), &rep) == Progress::Done);
    CHECK_FALSE(rep.newer_minor);
    const PerfState& pf = proj(1).perf();
    CHECK(pf.fx[0] == 0);
    CHECK(pf.row[0] == 0);
    CHECK(pf.value[0][1] == 0);  // slot 0 had row 2 and values: reset with its FX
    CHECK(pf.fx[1] == 2);
    CHECK(pf.row[1] == 0);
    CHECK(pf.fx[4] == 5);
    CHECK(pf.row[4] == 1);       // the others are as they were
    CHECK(proj(1).check_invariants());
  }
  refused("MIDI 2.0", with(kChunkMidi, [](Bytes& p) { p[0] = 2; p.push_back(0); }), Error::NewerMajor);
  refused("APPM 2.0", with(kChunkAppm, [](Bytes& p) { p[0] = 2; p.resize(p.size() + 100); }), Error::NewerMajor);

  // Order: after LRNB, MIDI, USBS, MIXR, PERF, APPM, each once, before the CRC; an unknown chunk may sit anywhere after PHDR.
  std::vector<RawChunk> out;
  for (const RawChunk& c : cs) {
    if (c.id == kChunkMixr) {
      continue;
    }
    out.push_back(c);
    if (c.id == kChunkPerf) {
      out.push_back(*find(cs, kChunkMixr));  // MIXR after PERF
    }
  }
  refused("MIXR after PERF", out);
  out = cs;
  out.push_back(*find(cs, kChunkMidi));  // a second MIDI, after the unknown chunks
  refused("MIDI twice", out);
  out.clear();
  for (const RawChunk& c : cs) {
    out.push_back(c);
    if (c.id == kChunkPhdr) {
      out.push_back(*find(cs, kChunkMidi));  // MIDI before TRAK
    }
  }
  refused("MIDI right after PHDR", out);
  out.clear();
  for (const RawChunk& c : cs) {
    out.push_back(c);
    if (c.id == kChunkMixr) {
      out.push_back(RawChunk{kChunkMidi + 1, {1, 2, 3}});  // an unknown chunk between the state chunks
    }
  }
  NEED(test::load(test::arena(1), build(out), &rep) == Progress::Done);
  CHECK(rep.unknown_kept == 3u);  // XTRA, EMPT and this one

  // A newer minor: the fields of 1.0 are read, the appended bytes skipped (at any length), the report says so, and the next save writes 1.0.
  // (USBS is not here: since OS-USBOUTS its 1.1 appends the channel count, so a 1.0 body with a minor of 1.1 or 1.2 is not a valid chunk; the newer-minor USBS cases are in
  // tests/o3project_roundtrip.py, "USBS 1.2 with appended fields", read by the C++ side through o3pj_tool)
  for (std::uint32_t id : {kChunkMidi, kChunkMixr, kChunkPerf, kChunkAppm}) {
    for (std::size_t tail : {std::size_t{1}, std::size_t{100}, std::size_t{20000}}) {
      INFO("chunk " << id << " tail " << tail);
      std::vector<RawChunk> next = cs;
      for (RawChunk& c : next) {
        if (c.id == id) {
          c.payload[1] = (id == kChunkPerf || id == kChunkAppm) ? 2 : 1;  // (PERF is at 1.1 since OS-PERFFX P1 and APPM since OS-APPUX S3: their newer minor is 1.2)
          c.payload.insert(c.payload.end(), tail, 0xAB);
        }
      }
      NEED(test::load(test::arena(1), build(next), &rep) == Progress::Done);
      CHECK(rep.newer_minor);
      Bytes saved;
      NEED(test::save(proj(1), saved) == Progress::Done);
      CHECK(saved == file);
    }
  }
}

TEST_CASE("project state: a state chunk with the default values is still a chunk (the part is present), and the writer writes only what is present") {
  Project& p = proj(0);
  p.reset();
  MixState x;
  init_mix_state(x);
  NEED(p.set_mix(x) == Status::Ok);  // present, with the defaults
  Bytes out;
  NEED(test::save(p, out) == Progress::Done);
  std::vector<RawChunk> cs = test::split_chunks(out);
  CHECK(cs.size() == 5u);  // PHDR TRAK MODR SONG MIXR
  CHECK(cs.back().id == kChunkMixr);
  Report rep;
  NEED(test::load(test::arena(1), out, &rep) == Progress::Done);
  CHECK(proj(1).state_present() == Project::kStateMix);
  CHECK(proj(1).mix() == x);
  Bytes again;
  NEED(test::save(proj(1), again) == Progress::Done);
  CHECK(again == out);
  CHECK(out.size() == 186u + 8u + 25u + 1u);  // the chunk header, the payload, the pad byte
}

TEST_CASE("project state: the unknown pool of an older build holds every state chunk next to a full OG import (so a newer file is kept, not dropped)") {
  // An older build keeps the five chunks as unknown chunks (16 chunks, 16,384 bytes) and writes them back: at their largest they are 1,152 bytes in five chunks;
  // the OG chunks that share the pool are OGPM (at most 970) and OGPN (at most 12,822). docs/formats/o3-project.md section 10. OS-BUG-PASTE24: LAW (3 bytes, one more chunk) joins them.
  constexpr std::size_t state_max = 20 + 27 + 25 + 99 + 981;
  constexpr std::size_t law_max = 3;
  CHECK(state_max == 1152u);
  CHECK(state_max + law_max + 970u + 12822u <= kUnknownPoolBytes);
  CHECK(5u + 1u + 2u + 2u <= kMaxUnknownChunks);
}

TEST_CASE("project state OS-PRFDEFAULTS: a new project holds the default values, a file that stores 0s loads them as 0s, and the file is byte for byte what it was") {
  // new: init_perf_state gives every slot its FX's three defaults (the table of docs/specs/OS-PERFFX.md section 9)
  PerfState fresh;
  init_perf_state(fresh);
  for (std::uint32_t s = 0; s < kPerfSlots; ++s) {
    for (std::uint32_t r = 0; r < kPerfStoredRows; ++r) {
      CHECK(fresh.value[s][r] == kPerfDefaultValues[fresh.fx[s]][r]);
    }
  }
  CHECK(fresh.value[3][0] == -30);  // LOWPASS
  CHECK(fresh.value[5][2] == 70);   // BANDPASS
  CHECK(fresh.row[0] == 0);
  // the saved project of a build before the table: every value 0, rows chosen. It loads as it is (no default is put back over a 0) and saves the same bytes.
  test::fill_state(proj(2));
  PerfState zeros = proj(2).perf();
  for (std::uint32_t s = 0; s < kPerfSlots; ++s) {
    zeros.fx[s] = static_cast<std::uint8_t>(s + 1u);
    zeros.row[s] = static_cast<std::uint8_t>(s % 4u);
    for (std::uint32_t r = 0; r < kPerfStoredRows; ++r) {
      zeros.value[s][r] = 0;
    }
  }
  CHECK(proj(2).set_perf(zeros) == Status::Ok);
  Bytes file;
  NEED(test::save(proj(2), file) == Progress::Done);
  NEED(test::load(test::arena(1), file) == Progress::Done);
  CHECK(proj(1).perf() == zeros);
  Bytes again;
  NEED(test::save(proj(1), again) == Progress::Done);
  CHECK(again == file);
}
