// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "support/need.h"
#include "support/og_fixture.h"

#include <o3os/formats/native.h>
#include <o3os/formats/og.h>
#include <o3os/model/project.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace o3os;
using namespace o3os::test::og;
using formats::Progress;
using o3os::test::MemorySource;
namespace og_ns = o3os::formats::og;

// OG -> model -> OG over a whole synthetic project folder (OS-004b "done when"): byte-identical for project v16,
// 12-track pattern v5, .pti v9 and PAMD, and for everything that is not, a list of exactly which bytes Export
// normalises, shown by feeding junk into them and getting the same file out.

namespace {

struct InstrumentFile {
  unsigned slot;
  InstrumentSpec spec;
};

struct Folder {
  ProjectSpec project;
  std::vector<std::pair<unsigned, PatternSpec>> patterns;  // id, pattern
  std::vector<InstrumentFile> instruments;
  std::vector<std::string> names;                          // PAMD record i
};

Folder canonical_folder(unsigned project_version = 16) {
  Folder f;
  // (the target has 4 MiB of RAM, two arenas and a small heap: one pattern there, four on the host)
  const unsigned ids[] = {1, 12, 2, 3};
#if defined(__arm__)
  const std::size_t count = 1;
#else
  const std::size_t count = 4;
#endif
  std::uint32_t seed = 40;
  for (std::size_t i = 0; i < count; ++i) f.patterns.push_back({ids[i], rich_pattern(12, 5, seed++)});
  f.project.version = project_version;
  f.project.playlist = {1, 2, 2, 3, 12};
  for (const auto& p : f.patterns) f.project.bitmask[p.first - 1] = usage_byte(p.second);
  InstrumentSpec a;
  a.frames = 1000;
  a.file_name = "Kick 808.wav";
  InstrumentSpec b;
  b.frames = 1;
  b.file_name = "x";
  InstrumentSpec c;
  c.frames = 2500;
  c.file_name = "Long Name With Spaces.wav";
  f.instruments = {{0, a}, {5, b}, {47, c}};
  f.names.assign(256, "");
  const char* titles[] = {"Intro", "Verse A", "Chorus", "Outro, a name of thirty chars"};
  for (std::size_t i = 0; i < f.patterns.size(); ++i) f.names[f.patterns[i].first - 1] = titles[i];
  return f;
}

struct Files {
  Bytes project;
  std::vector<Bytes> patterns;
  std::vector<Bytes> instruments;
  Bytes pamd;
};

Files files_of(const Folder& f) {
  Files out;
  out.project = project_bytes(f.project);
  for (const auto& p : f.patterns) out.patterns.push_back(pattern_bytes(p.second));
  for (const InstrumentFile& i : f.instruments) out.instruments.push_back(instrument_bytes(i.spec));
  out.pamd = pamd_bytes(f.names);
  return out;
}

// Imports a folder's files in the order of an import into `a`.
void import_all(const Folder& f, const Files& files, model::ModelArena& a) {
  static og_ns::ProjectReader project;
  static og_ns::PatternReader pattern;
  static og_ns::InstrumentReader pti(true);
  static og_ns::PamdReader pamd;
  NEED(run_reader(project, files.project, a) == Progress::Done);
  for (std::size_t i = 0; i < f.patterns.size(); ++i) {
    formats::ReadParams p;
    p.index = static_cast<std::uint16_t>(f.patterns[i].first);
    NEED(run_reader(pattern, files.patterns[i], a, p) == Progress::Done);
  }
  for (std::size_t i = 0; i < f.instruments.size(); ++i) {
    formats::ReadParams p;
    p.index = static_cast<std::uint16_t>(f.instruments[i].slot);
    NEED(run_reader(pti, files.instruments[i], a, p) == Progress::Done);
  }
  NEED(run_reader(pamd, files.pamd, a) == Progress::Done);
}

// Exports the model back into files, in the same shape.
void export_all(const Folder& f, const model::Project& p, Files& out, og_ns::ExportDrops* drops = nullptr,
                std::uint8_t target = og_ns::kTargetOg190) {
  og_ns::ProjectWriter pw;
  og_ns::PatternWriter tw;
  og_ns::InstrumentWriter iw;
  og_ns::PamdWriter nw;
  formats::WriteParams project_params;
  project_params.target = target;
  NEED(run_writer(pw, p, out.project, project_params) == Progress::Done);
  if (drops != nullptr) drops->add(pw.export_report().drops);
  for (const auto& pat : f.patterns) {
    formats::WriteParams wp;
    wp.target = target;  // the other writers have no such choice and ignore it
    wp.index = static_cast<std::uint16_t>(pat.first);
    out.patterns.emplace_back();
    NEED(run_writer(tw, p, out.patterns.back(), wp) == Progress::Done);
    if (drops != nullptr) drops->add(tw.export_report().drops);
  }
  for (const InstrumentFile& i : f.instruments) {
    const Bytes in = instrument_bytes(i.spec);
    const Bytes pcm(in.begin() + 392, in.end());
    MemorySource src(pcm);
    formats::WriteParams wp;
    wp.index = static_cast<std::uint16_t>(i.slot);
    wp.pcm = &src;
    wp.pcm_frames = i.spec.frames;
    wp.target = target;
    out.instruments.emplace_back();
    NEED(run_writer(iw, p, out.instruments.back(), wp) == Progress::Done);
    if (drops != nullptr) drops->add(iw.export_report().drops);
  }
  formats::WriteParams names_params;
  names_params.target = target;
  NEED(run_writer(nw, p, out.pamd, names_params) == Progress::Done);
}

bool same(const Files& a, const Files& b) {
  return a.project == b.project && a.patterns == b.patterns && a.instruments == b.instruments && a.pamd == b.pamd;
}

// The model as the native format writes it, as a length and a CRC-32 of the bytes (no copy of them: the target
// has a small heap): one comparison of everything in it.
struct Digest {
  std::uint32_t crc;
  std::size_t size;
  bool operator==(const Digest& o) const { return crc == o.crc && size == o.size; }
};

class DigestSink final : public core::ByteSink {
 public:
  core::IoResult write(const void* src, std::size_t n) noexcept override {
    crc_.update(static_cast<const std::uint8_t*>(src), n);
    size_ += n;
    return {n, core::IoStatus::Ok};
  }
  Digest digest() const { return {crc_.value(), size_}; }

 private:
  formats::Crc32 crc_;
  std::size_t size_ = 0;
};

Digest native_digest(const model::Project& p) {
  formats::native::Writer w;
  DigestSink sink;
  w.begin(p, sink);
  while (w.step(4096) == Progress::Running) {}
  return sink.digest();
}

Bytes native_bytes(const model::Project& p) {
  formats::native::Writer w;
  test::VectorSink sink;
  w.begin(p, sink);
  while (w.step(4096) == Progress::Running) {}
  return sink.bytes;
}

}  // namespace

TEST_CASE("og roundtrip: a synthetic v16 project folder comes back byte for byte") {
  const Folder f = canonical_folder();
  const Files in = files_of(f);
  CHECK(in.project.size() == 2320);
  CHECK(in.patterns[0].size() == 9260);
  CHECK(in.instruments[0].size() == 392 + 2000);
  CHECK(in.pamd.size() == 12816);
  model::ModelArena& a = arena(0);
  import_all(f, in, a);
  CHECK(a.project.pattern_count() == f.patterns.size());
  CHECK(a.project.instrument_count() == 16 + 3);
  CHECK(a.project.check_invariants());
  og_ns::ExportDrops drops;
  Files out;
  export_all(f, a.project, out, &drops);
  CHECK(out.project == in.project);
  for (std::size_t i = 0; i < in.patterns.size(); ++i) CHECK_MESSAGE(out.patterns[i] == in.patterns[i], "pattern " << i);
  for (std::size_t i = 0; i < in.instruments.size(); ++i) {
    CHECK_MESSAGE(out.instruments[i] == in.instruments[i], "instrument " << i);
  }
  CHECK(out.pamd == in.pamd);
  CHECK(!drops.any_lost());
  CHECK(drops.volume_as_fx18 == 0);

  // read the exported files again: the same model (every byte of it, through the native format)
  model::ModelArena& b = arena(1);
  import_all(f, out, b);
  CHECK(native_digest(a.project) == native_digest(b.project));
  // and through the native format and back
  {
    const Bytes nat = native_bytes(a.project);
    formats::native::Reader r;
    MemorySource src(nat);
    r.begin(src, b);
    while (r.step(4096) == Progress::Running) {}
    NEED(r.progress() == Progress::Done);
    CHECK(native_digest(b.project) == native_digest(a.project));
  }
  Files out2;
  export_all(f, b.project, out2);
  CHECK(same(out2, in));
}

TEST_CASE("og roundtrip: a synthetic v17 project folder (the layout assumed for 1.9.2) comes back byte for byte with the 1.9.2 target") {
  Folder f = canonical_folder(17);
  f.project.byte_80c = 0x5A;  // the one thing that is new in a 1.9.2 project: carried, written back
  const Files in = files_of(f);
  CHECK(in.project.size() == 2324);
  CHECK(in.project[0x80C] == 0x5A);
  model::ModelArena& a = arena(0);
  import_all(f, in, a);
  CHECK(a.project.pattern_count() == f.patterns.size());
  CHECK(a.project.check_invariants());
  og_ns::ExportDrops drops;
  Files out;
  export_all(f, a.project, out, &drops, og_ns::kTargetOg192);
  CHECK(out.project == in.project);
  CHECK(out.patterns == in.patterns);  // pattern 5, instrument 9 and PAMD 1 are the same for both targets
  CHECK(out.instruments == in.instruments);
  CHECK(out.pamd == in.pamd);
  CHECK(!drops.any_lost());

  // read the exported files again: the same model; and the 1.9.0 target writes the same files but the project's
  model::ModelArena& b = arena(1);
  import_all(f, out, b);
  CHECK(native_digest(a.project) == native_digest(b.project));
  Files old_target;
  og_ns::ExportDrops old_drops;
  export_all(f, a.project, old_target, &old_drops, og_ns::kTargetOg190);
  CHECK(old_target.project.size() == 2320);
  CHECK(old_target.patterns == in.patterns);
  CHECK(old_target.instruments == in.instruments);
  CHECK(old_target.pamd == in.pamd);
  CHECK(old_drops.project_settings == 1);  // the byte at 0x80C has no home in v16 (counted, not silent)
  CHECK(old_drops.any_lost());
}

TEST_CASE("og roundtrip: the bytes Export normalises are exactly the ones nobody reads") {
  // The same folder, with junk in every byte Export rebuilds or defaults (the project's session state and padding,
  // the header bytes Export rebuilds, the instruments' pointers and padding):
  // what comes out is the canonical folder. The settings groups and the PAMD file are not in this list: they are
  // carried (see og_settings_test.cpp).
  const Folder clean = canonical_folder();
  Folder junk = clean;
  junk.project.fill = ProjectSpec::Fill::Junk;
  junk.project.junk = 777;
  const Files want = files_of(clean);
  Files in = files_of(junk);
  CHECK(in.project != want.project);
  for (std::size_t i = 0; i < junk.patterns.size(); ++i) {
    PatternSpec& p = junk.patterns[i].second;
    p.magic[0] = 'P';
    p.magic[1] = 'M';
    p.fw[0] = 1; p.fw[1] = 3; p.fw[2] = 1; p.fw[3] = 7;
    p.pad[0] = 0xA5;
    p.pad[1] = 0x5A;
    for (unsigned k = 0; k < 12; ++k) p.head_tail[k] = static_cast<std::uint8_t>(0x66 + k);
    in.patterns[i] = pattern_bytes(p);
    in.patterns[i][2] = 9;      // the type field
    in.patterns[i][12] = 0x34;  // the size field
    in.patterns[i][13] = 0x12;
    seal(in.patterns[i]);
    CHECK(in.patterns[i] != want.patterns[i]);
  }
  for (std::size_t i = 0; i < junk.instruments.size(); ++i) {
    junk.instruments[i].spec.canonical = false;
    in.instruments[i] = instrument_bytes(junk.instruments[i].spec);
    in.instruments[i][4] = 1; in.instruments[i][5] = 5; in.instruments[i][6] = 0; in.instruments[i][7] = 77;  // firmware
    in.instruments[i][12] = 0x99;                                                                            // size field
    p32(in.instruments[i], 388, formats::crc32(in.instruments[i].data(), 388));
    CHECK(in.instruments[i] != want.instruments[i]);
  }
  in.pamd = pamd_bytes(junk.names);
  in.pamd[6] = in.pamd[7] = 0x99;
  p32(in.pamd, 12, 0xCAFEF00Du);
  p32(in.pamd, 8, 12345);
  for (unsigned i = 0; i < 256; ++i) {
    for (unsigned k = 31; k < 50; ++k) in.pamd[16 + 50u * i + k] = static_cast<std::uint8_t>(0xE0 + k);
  }
  CHECK(in.pamd != want.pamd);

  model::ModelArena& a = arena(0);
  import_all(junk, in, a);
  Files out;
  export_all(junk, a.project, out);
  CHECK(out.project == want.project);
  for (std::size_t i = 0; i < want.patterns.size(); ++i) CHECK(out.patterns[i] == want.patterns[i]);
  for (std::size_t i = 0; i < want.instruments.size(); ++i) CHECK(out.instruments[i] == want.instruments[i]);
  CHECK(out.pamd == in.pamd);  // the PAMD file is carried verbatim, junk and all: nothing in it is normalised
}

TEST_CASE("og roundtrip: a v6 pattern goes through the native format unchanged, the micro-move in FX 2 (OS-CEPAT S1)") {
  // a rich 16-track v6 pattern, and one crafted step that shows where the micromove lands
  PatternSpec6 spec = rich_pattern6(16, 6, 61);
  spec.step[0][0][0] = 60; spec.step[0][0][1] = 5;
  for (unsigned k = 2; k < 6; ++k) spec.step[0][0][k] = 0;  // free FX columns for the micromove
  spec.step[0][0][6] = 100; spec.step[0][0][7] = 50;
  const Bytes in = pattern6_bytes(spec);
  CHECK(in.size() == 16432);
  model::ModelArena& a = arena(0);
  a.project.reset();  // the arenas are shared between test cases
  {
    static og_ns::PatternReader pattern;
    formats::ReadParams p;
    p.index = 3;
    NEED(run_reader(pattern, in, a, p) == Progress::Done);
    NEED(pattern.import_report().tracks == 16);
  }
  CHECK(a.project.check_invariants());
  const model::Pattern* pa = a.project.pattern(3);
  NEED(pa != nullptr);
  CHECK(pa->steps[0][0].vol == 127);                            // 100 -> floor(100 * 127 / 100)
  CHECK(pa->steps[0][0].fx[0].type == 2);                       // the micromove, as FX 2
  CHECK(pa->steps[0][0].fx[0].value == 50);

  // v6 -> model -> .o3pj -> model is equal, every track and every step
  const Bytes nat = native_bytes(a.project);
  model::ModelArena& b = arena(1);
  b.project.reset();
  {
    formats::native::Reader r;
    MemorySource src(nat);
    r.begin(src, b);
    while (r.step(4096) == Progress::Running) {}
    NEED(r.progress() == Progress::Done);
  }
  CHECK(native_digest(a.project) == native_digest(b.project));
  const model::Pattern* pb = b.project.pattern(3);
  NEED(pb != nullptr);
  for (unsigned t = 0; t < model::kMaxTracks; ++t) {
    CHECK(pa->track_length[t] == pb->track_length[t]);
    CHECK(std::memcmp(pa->steps[t], pb->steps[t], sizeof pa->steps[t]) == 0);
  }
}

TEST_CASE("og roundtrip: a v6 pattern exported to v5 loses only what is counted (OS-CEPAT S1)") {
  PatternSpec6 spec = empty_pattern6(12, 6);
  auto set = [&spec](unsigned s, std::uint8_t note, std::uint8_t instr, std::uint8_t t0, std::uint8_t v0,
                     std::uint8_t t1, std::uint8_t v1, std::int8_t vol, std::int8_t m) {
    std::uint8_t* st = spec.step[0][s];
    st[0] = note; st[1] = instr;
    st[2] = t0; st[3] = v0; st[4] = t1; st[5] = v1;
    st[6] = static_cast<std::uint8_t>(vol);
    st[7] = static_cast<std::uint8_t>(m);
  };
  set(0, 60, 5, 0, 0, 0, 0, 100, -1);    // vol -> FX 18 in the free slot
  set(1, 60, 5, 4, 50, 0, 0, 50, 100);   // m -> FX 2 in slot 1; vol: no slot left, dropped
  set(2, 60, 5, 2, 33, 0, 0, -1, 100);   // FX 2 already there: m superseded
  set(3, 60, 5, 4, 50, 5, 60, -1, 100);  // no free slot: m dropped (counted on import)
  set(4, 60, 5, 16, 16, 0, 0, -1, 100);  // Random value 16 in the other slot: m dropped (counted)
  set(5, 0xFF, 0, 0, 0, 0, 0, 50, -1);   // vol on a no-note step: still exported as FX 18
  set(6, 60, 5, 43, 8, 0, 0, -1, -1);    // OG 43 -> model 96: dropped as ce_fx on export
  const Bytes in = pattern6_bytes(spec);
  model::ModelArena& a = arena(0);
  a.project.reset();  // the arenas are shared between test cases
  og_ns::ImportReport rep;
  {
    static og_ns::PatternReader pattern;
    formats::ReadParams p;
    p.index = 1;
    NEED(run_reader(pattern, in, a, p) == Progress::Done);
    rep = pattern.import_report();
  }
  CHECK(rep.micromoves_dropped == 2);  // steps 3 and 4
  CHECK(rep.values_clamped == 0);

  Bytes out;
  {
    static og_ns::PatternWriter w;
    formats::WriteParams p;
    p.index = 1;
    NEED(run_writer(w, a.project, out, p) == Progress::Done);
    const og_ns::ExportDrops& d = w.export_report().drops;
    CHECK(d.volume_as_fx18 == 2);   // steps 0 and 5
    CHECK(d.volume_dropped == 1);   // step 1
    CHECK(d.ce_fx == 1);            // step 6: model 96
    CHECK(d.tracks_beyond_12 == 0);
    CHECK(d.core_fx == 0);
    CHECK(d.engine_fx == 0);
  }
  CHECK(out.size() == 9260);  // Export still writes v5, 12 tracks
  auto at = [&out](unsigned s) { return &out[29 + 6 * s]; };
  auto eq = [](const std::uint8_t* p, std::initializer_list<std::uint8_t> v) {
    return std::equal(v.begin(), v.end(), p);
  };
  CHECK(eq(at(0), {60, 5, 18, 100, 0, 0}));    // vol 100 -> FX 18 = 100
  CHECK(eq(at(1), {60, 5, 4, 50, 2, 100}));     // m as FX 2; vol dropped
  CHECK(eq(at(2), {60, 5, 2, 33, 0, 0}));       // superseded: the FX column wins
  CHECK(eq(at(3), {60, 5, 4, 50, 5, 60}));      // m dropped on import, nothing to write
  CHECK(eq(at(4), {60, 5, 16, 16, 0, 0}));      // Random 16 stays, m was dropped
  CHECK(eq(at(5), {255, 0, 18, 50, 0, 0}));     // vol 50 -> floor(50*127/100)=63 -> FX 18 = 50
  CHECK(eq(at(6), {60, 5, 0, 0, 0, 0}));        // tracker-ce FX: written as none, counted
  // the export reads back, and what it loses is exactly the counted items: vol (unset in v5, FX 18 instead)
  // and the dropped micro-moves; a second export is byte-identical
  model::ModelArena& b = arena(1);
  b.project.reset();
  {
    static og_ns::PatternReader pattern;
    formats::ReadParams p;
    p.index = 1;
    NEED(run_reader(pattern, out, b, p) == Progress::Done);
  }
  const model::Pattern* pa = a.project.pattern(1);
  const model::Pattern* pb = b.project.pattern(1);
  NEED(pa != nullptr && pb != nullptr);
  for (unsigned t = 0; t < model::kMaxTracks; ++t) CHECK(pa->track_length[t] == pb->track_length[t]);
  for (unsigned s = 0; s < 128; ++s) {
    const model::Step& x = pa->steps[0][s];
    const model::Step& y = pb->steps[0][s];
    CHECK(x.note == y.note);
    CHECK(x.instr == y.instr);
    CHECK(y.vol == model::kVolumeUnset);  // v5 has no volume column
    if (s == 0) {
      CHECK(y.fx[0].type == 18);
      CHECK(y.fx[0].value == 100);
      CHECK(y.fx[1].type == 0);
    }
    if (s == 1) {
      CHECK(y.fx[0].type == 4);
      CHECK(y.fx[0].value == 50);
      CHECK(y.fx[1].type == 2);
      CHECK(y.fx[1].value == 100);
    }
    if (s == 5) {
      CHECK(y.fx[0].type == 18);
      CHECK(y.fx[0].value == 50);
    }
    if (s == 6) {  // ce_fx dropped
      CHECK(y.fx[0].type == 0);
      CHECK(y.fx[1].type == 0);
    }
    if (s == 2 || s == 3 || s == 4) {
      CHECK(y.fx[0].type == x.fx[0].type);
      CHECK(y.fx[0].value == x.fx[0].value);
      CHECK(y.fx[1].type == x.fx[1].type);
      CHECK(y.fx[1].value == x.fx[1].value);
    }
  }
  Bytes out2;
  {
    static og_ns::PatternWriter w;
    formats::WriteParams p;
    p.index = 1;
    NEED(run_writer(w, b.project, out2, p) == Progress::Done);
  }
  CHECK(out2 == out);
}

TEST_CASE("og roundtrip: older projects are exported as v16 with what the older version held") {
  // 1.1 (v6), 1.4 (v12) and 1.6 (v14) projects with 8-track patterns of versions 1, 3 and 4
  struct Case { unsigned project; unsigned pattern; };
  for (const Case c : {Case{6, 1}, Case{12, 3}, Case{14, 4}}) {
    Folder old;
    old.project.version = c.project;
    old.project.fill = ProjectSpec::Fill::Junk;
    old.project.playlist = {1, 3};
    for (std::uint32_t k = 0; k < 2; ++k) old.patterns.push_back({1 + 2 * k, rich_pattern(8, c.pattern, 60 + k)});
    for (const auto& p : old.patterns) old.project.bitmask[p.first - 1] = usage_byte(p.second);
    for (unsigned i = 8; i < 16; ++i) old.project.names[i] = "";  // versions before 16 have no names for tracks 9-16
    old.names.assign(256, "");
    InstrumentSpec i0;
    i0.version = c.project >= 12 ? 8 : 4;
    i0.pti = false;
    i0.canonical = false;
    old.instruments = {};  // .mti are read by another reader: covered in og_instrument_test
    const Files in = files_of(old);
    model::ModelArena& a = arena(0);
    import_all(old, in, a);
    CHECK(a.project.active_tracks() == 8);
    // the expectation: the same values as a v16 project, the four new tracks empty, and the usage bitmask rebuilt
    Folder want = old;
    want.project.version = 16;
    want.project.fill = ProjectSpec::Fill::Defaults;
    for (auto& p : want.patterns) {
      PatternSpec& s = p.second;
      s.tracks = 12;
      s.version = 5;
      for (unsigned t = 8; t < 12; ++t) {
        s.length[t] = s.length[0];
        for (unsigned k = 0; k < 128; ++k) {
          std::memset(s.step[t][k], 0, 6);
          s.step[t][k][0] = 0xFF;
        }
      }
      std::memcpy(s.head_tail, PatternSpec{}.head_tail, 12);
      s.fw[0] = 1; s.fw[1] = 9; s.fw[2] = 0; s.fw[3] = 1;
      s.pad[0] = s.pad[1] = 0;
      s.magic[0] = 'K';
      s.magic[1] = 'S';
    }
    const Files expect = files_of(want);
    Files out;
    export_all(old, a.project, out);
    CHECK_MESSAGE(out.project == expect.project, "project v" << c.project);
    for (std::size_t i = 0; i < expect.patterns.size(); ++i) {
      CHECK_MESSAGE(out.patterns[i] == expect.patterns[i], "project v" << c.project << " pattern " << i);
    }
  }
}
