// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "support/need.h"
#include "support/og_fixture.h"

#include <o3os/formats/native.h>
#include <o3os/formats/og.h>
#include <o3os/model/project.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace o3os;
using namespace o3os::test::og;
using formats::Progress;
using o3os::test::MemorySource;
namespace og_ns = o3os::formats::og;
namespace fs = std::filesystem;

// Real OG project files (a local `fixtures/` directory, never committed; the tests skip when it is absent): every project folder is imported,
// exported and imported again. Nothing in them is ever printed or copied: only counts and byte offsets. The test
// skips when the fixtures are absent. O3_FIXTURES_DIR names the `sd` directory; the default is `fixtures/sd` in the
// nearest ancestor of this file that has one (a build tree may sit inside another checkout).

namespace {

fs::path find_fixtures() {
  if (const char* env = std::getenv("O3_FIXTURES_DIR")) {
    return fs::path(env);
  }
  fs::path p = fs::absolute(fs::path(__FILE__)).parent_path();
  while (true) {
    if (fs::is_directory(p / "fixtures" / "sd")) return p / "fixtures" / "sd";
    if (!p.has_parent_path() || p == p.parent_path()) return {};
    p = p.parent_path();
  }
}

Bytes read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct Counts {
  unsigned groups = 0, projects_ok = 0, projects_err = 0;
  unsigned patterns_ok = 0, patterns_err = 0, patterns8 = 0, patterns12 = 0;
  unsigned mti_ok = 0, pti_ok = 0, instruments_skipped = 0, instruments_err = 0;
  unsigned pamd_ok = 0, pamd_err = 0;
  unsigned sanitised = 0, crc_warn = 0, unverified = 0;
  unsigned fx_above_42 = 0;  // OS-CEFXIDS: raw OG FX type bytes above 42, the ids the S0 map now reads as tracker-ce FX (expected 0 on fixtures-1)
  unsigned pattern_tracks_identical = 0, bitmask_agree = 0, bitmask_total = 0;
  unsigned projects_stable = 0;
  unsigned groups_identical = 0, settings_chunks = 0, pamd_identical = 0;
};

struct Item {
  fs::path path;
  og_ns::FileName name;
};

std::vector<Item> list(const fs::path& dir, og_ns::FileClass cls) {
  std::vector<Item> out;
  if (!fs::is_directory(dir)) return out;
  for (const fs::directory_entry& e : fs::directory_iterator(dir)) {
    if (!e.is_regular_file()) continue;
    const og_ns::FileName n = og_ns::classify_file_name(e.path().filename().string().c_str());
    if (n.file_class == cls) out.push_back({e.path(), n});
  }
  std::sort(out.begin(), out.end(), [](const Item& a, const Item& b) { return a.name.index < b.name.index; });
  return out;
}

Bytes native_bytes(const model::Project& p) {
  formats::native::Writer w;
  test::VectorSink sink;
  w.begin(p, sink);
  while (w.step(4096) == Progress::Running) {}
  return sink.bytes;
}

// File offsets whose bytes a pattern export may change: fw (4-7), version (8-11), size and padding (12-15) and
// the head's tempo, swing and reserved bytes (16-27); the CRC is the last four.
bool pattern_header_byte(std::size_t i) { return i >= 4 && i < 28; }

// Offsets of a 392-byte instrument that an export may change (docs/formats/og.md): header, padding, the live
// pointer, the frame count, the wavetable window number, reserved bytes, junk behind the name's NUL, the
// overdrive and bit depth of versions that had none, and the CRC. (279 is the play-thru flag since OS-CEPLAYTHRU P1:
// the fixtures hold 0 there, measured, and the export writes the model's 0.)
bool instrument_free_byte(std::size_t i) {
  return (i >= 4 && i < 16) || (i >= 17 && i < 20) || (i >= 21 && i < 53) || (i >= 53 && i < 76) || i == 77 ||
         (i >= 86 && i < 88) || i == 273 || (i >= 385 && i < 388) || i >= 388;
}

}  // namespace

TEST_CASE("og fixtures: every project of the fixture card imports, exports and imports again") {
  const fs::path root = find_fixtures();
  if (root.empty() || !fs::is_directory(root)) {
    std::printf("og fixture survey: no fixtures found, skipped\n");
    return;
  }
  Counts c;
  static og_ns::ProjectReader project_reader;
  static og_ns::PatternReader pattern_reader;
  static og_ns::InstrumentReader mti_reader(false);
  static og_ns::InstrumentReader pti_reader(true);
  static og_ns::PamdReader pamd_reader;
  std::set<std::size_t> changed_instrument_bytes;
  unsigned instrument_files = 0;
  unsigned instrument_identical = 0;

  std::vector<fs::path> groups;
  for (const fs::directory_entry& e : fs::directory_iterator(root)) {
    if (e.is_directory()) groups.push_back(e.path());
  }
  std::sort(groups.begin(), groups.end());

  for (const fs::path& group : groups) {
    const fs::path project_path = group / "project.mt";
    if (!fs::is_regular_file(project_path)) continue;
    ++c.groups;
    const std::string label = group.filename().string();  // "v1.4.0-projver12": a version, not a name
    model::ModelArena& a = arena(0);
    og_ns::ImportReport rep;

    // ---- import: project settings, patterns, instruments, names
    const Bytes project_file = read_file(project_path);
    if (run_reader(project_reader, project_file, a) != Progress::Done) {
      ++c.projects_err;
      CHECK_MESSAGE(false, label << ": project: " << project_reader.report().detail);
      continue;
    }
    rep = project_reader.import_report();
    ++c.projects_ok;
    c.sanitised += rep.sanitised;
    c.crc_warn += rep.crc_mismatch ? 1u : 0u;
    c.unverified += rep.unverified_version ? 1u : 0u;
    CHECK_MESSAGE(!rep.unverified_version, label);
    CHECK_MESSAGE(!rep.crc_mismatch, label);

    std::vector<Item> patterns = list(group / "patterns", og_ns::FileClass::Pattern);
    std::vector<Bytes> pattern_files;
    for (const Item& it : patterns) {
      pattern_files.push_back(read_file(it.path));
      formats::ReadParams p;
      p.index = it.name.index;
      if (run_reader(pattern_reader, pattern_files.back(), a, p) != Progress::Done) {
        ++c.patterns_err;
        CHECK_MESSAGE(false, label << ": pattern " << it.name.index << ": " << pattern_reader.report().detail);
        continue;
      }
      ++c.patterns_ok;
      (pattern_reader.import_report().tracks == 8 ? c.patterns8 : c.patterns12) += 1;
      c.sanitised += pattern_reader.import_report().sanitised;
      CHECK(!pattern_reader.import_report().unverified_version);
      {  // the FX type bytes of the raw file, before any map: the survey that says no fixture holds an id above 42
        const Bytes& pf = pattern_files.back();
        if (pf.size() == 6184 || pf.size() == 9260) {
          const unsigned tracks = pf.size() == 6184 ? 8u : 12u;
          for (unsigned t = 0; t < tracks; ++t) {
            for (unsigned s = 0; s < 128; ++s) {
              const std::size_t at = 28 + 769u * t + 1 + 6u * s;
              if (pf[at + 2] > 42) ++c.fx_above_42;
              if (pf[at + 4] > 42) ++c.fx_above_42;
            }
          }
        }
      }
    }

    std::vector<Item> mtis = list(group / "instruments", og_ns::FileClass::Mti);
    std::vector<Item> ptis = list(group / "instruments", og_ns::FileClass::Pti);
    std::vector<Bytes> mti_files, pti_files;
    for (int pass = 0; pass < 2; ++pass) {  // .mti first, .pti after: a .pti wins a slot it shares (PAR-009 3.4)
      std::vector<Item>& items = pass == 0 ? mtis : ptis;
      std::vector<Bytes>& store = pass == 0 ? mti_files : pti_files;
      for (const Item& it : items) {
        store.push_back(read_file(it.path));
        formats::ReadParams p;
        p.index = it.name.index;
        og_ns::InstrumentReader& r = pass == 0 ? mti_reader : pti_reader;
        if (run_reader(r, store.back(), a, p) != Progress::Done) {
          ++c.instruments_err;
          CHECK_MESSAGE(false, label << ": instrument slot " << it.name.index << ": " << r.report().detail);
          continue;
        }
        c.sanitised += r.import_report().sanitised;
        c.crc_warn += r.import_report().crc_mismatch ? 1u : 0u;
        if (r.import_report().instruments == 0) {
          ++c.instruments_skipped;
        } else {
          (pass == 0 ? c.mti_ok : c.pti_ok) += 1;
        }
      }
    }

    const fs::path pamd_path = group / "patterns" / "patternsMetadata";
    Bytes pamd;
    if (fs::is_regular_file(pamd_path)) {
      pamd = read_file(pamd_path);
      if (run_reader(pamd_reader, pamd, a) == Progress::Done) {
        ++c.pamd_ok;
      } else {
        ++c.pamd_err;
        CHECK_MESSAGE(false, label << ": pattern names: " << pamd_reader.report().detail);
      }
    }
    CHECK(a.project.check_invariants());

    // ---- export: the project file keeps everything the model holds
    og_ns::ProjectWriter pw;
    Bytes out_project;
    NEED(run_writer(pw, a.project, out_project) == Progress::Done);
    const unsigned v = project_file[8];
    const std::size_t name_at = v == 16 ? 0x80C : (v >= 11 ? 0x600 : (v >= 6 ? 0x4E0 : (v == 5 ? 0x4DC : (v == 4 ? 0x4D0 : (v == 3 ? 0x428 : 0x418)))));
    auto text = [](const Bytes& b, std::size_t at, std::size_t cap) {
      std::string s;
      for (std::size_t i = 0; i < cap && b[at + i] != 0; ++i) s += static_cast<char>(b[at + i]);
      return s;
    };
    CHECK_MESSAGE(std::equal(project_file.begin() + 0x10, project_file.begin() + 0x10 + 255, out_project.begin() + 0x10),
                  label << ": playlist");
    CHECK_MESSAGE(std::equal(project_file.begin() + 0x1C0, project_file.begin() + 0x1C6, out_project.begin() + 0x1C0),
                  label << ": tempo and pattern length");
    CHECK_MESSAGE(std::equal(project_file.begin() + 0x12C, project_file.begin() + 0x134, out_project.begin() + 0x12C),
                  label << ": track mutes");
    const bool name_same = text(project_file, name_at, 32).substr(0, 31) == text(out_project, 0x80C, 32);  // (never printed)
    CHECK_MESSAGE(name_same, label << ": name");
    if (v >= 4) {
      for (unsigned i = 0; i < 8; ++i) {
        const bool same = text(project_file, 0x428 + 21 * i, 21) == text(out_project, 0x428 + 21 * i, 21);
        CHECK_MESSAGE(same, label << ": track name " << i);
      }
    }
    if (v >= 6) {
      CHECK_MESSAGE(std::equal(project_file.begin() + 0x4D0, project_file.begin() + 0x4DE, out_project.begin() + 0x4D0),
                    label << ": volumes and mutes");
    }
    // the settings the model has no field for come back with the file (OGPM): every byte of every group the
    // version has. Versions before 16 keep two things elsewhere: the velocities of 1.2 and before go to the channel
    // records, the value 1.4.0.1 and before kept at 0x4F7 is at 0x309 (the files of 1.9 keep it there too).
    {
      bool same = true;
      for (const GroupRange& r : group_ranges(v)) {
        if ((v <= 7 && r.begin == 0x309) || ((v == 11 || v == 12) && r.begin == 0x4DE)) continue;
        for (std::size_t i = r.begin; i < r.end; ++i) same = same && project_file[i] == out_project[i];
      }
      if (v <= 7) {
        for (unsigned ch = 0; ch < 16; ++ch) same = same && out_project[0x4FD + 16 * ch + 1] == project_file[0x309 + ch];
      }
      if (v == 11 || v == 12) {
        for (std::size_t i = 0x4DE; i < 0x600; ++i) same = same && (i == 0x4F7 || project_file[i] == out_project[i]);
        same = same && out_project[0x4F7] == 0 && out_project[0x309] == project_file[0x4F7];
      }
      CHECK_MESSAGE(same, label << ": settings groups");
      c.groups_identical += same ? 1u : 0u;
      c.settings_chunks += a.project.unknown_count() - (pamd.empty() ? 0u : 1u);
    }
    // the usage bitmask rebuilt from the patterns is the one the fixture files hold (PAR-006 M3), pattern by pattern
    for (std::size_t i = 0; i < patterns.size(); ++i) {
      const unsigned id = patterns[i].name.index;
      ++c.bitmask_total;
      if (out_project[0x319 + id - 1] == project_file[0x319 + id - 1]) ++c.bitmask_agree;
    }
    CHECK_MESSAGE(std::equal(project_file.begin() + 0x319, project_file.begin() + 0x418, out_project.begin() + 0x319),
                  label << ": pattern usage bitmask");

    // ---- export patterns: the tracks come back byte for byte
    for (std::size_t i = 0; i < patterns.size(); ++i) {
      og_ns::PatternWriter tw;
      formats::WriteParams wp;
      wp.index = patterns[i].name.index;
      Bytes out;
      NEED(run_writer(tw, a.project, out, wp) == Progress::Done);
      const Bytes& in = pattern_files[i];
      const std::size_t tracks = in.size() == 6184 ? 8 : 12;
      bool identical = std::equal(in.begin() + 28, in.begin() + 28 + 769 * static_cast<std::ptrdiff_t>(tracks), out.begin() + 28);
      CHECK_MESSAGE(identical, label << ": pattern " << wp.index << " tracks");
      c.pattern_tracks_identical += identical ? 1u : 0u;
      if (in.size() == 9260) {  // the whole file but the header bytes 4..27 (pattern_header_byte) and the CRC
        for (std::size_t k = 0; k < in.size() - 4; ++k) {
          const bool same = in[k] == out[k];  // (a byte is never printed, only where it is)
          if (!pattern_header_byte(k)) CHECK_MESSAGE(same, label << ": pattern " << wp.index << " byte " << k);
        }
      }
      CHECK(tw.export_report().drops.engine_fx == 0);
      CHECK(tw.export_report().drops.ce_fx == 0);  // OS-CEFXIDS: no fixture pattern exports a tracker-ce FX
      CHECK(tw.export_report().drops.volume_dropped == 0);
    }

    // ---- export instruments, and compare with the originals as far as they are comparable
    std::vector<Bytes> exported_instruments;
    std::vector<unsigned> exported_slots;
    for (unsigned slot = 0; slot < 48; ++slot) {
      if (og_ns::export_class(a.project, slot) != og_ns::ExportClass::Pti) continue;
      og_ns::InstrumentWriter iw;
      // a PCM of one frame: the instrument is what is compared, and a .pti without PCM is no instrument
      const Bytes pcm{1, 2};
      MemorySource src(pcm);
      formats::WriteParams wp;
      wp.index = static_cast<std::uint16_t>(slot);
      wp.pcm = &src;
      wp.pcm_frames = 1;
      Bytes out;
      NEED(run_writer(iw, a.project, out, wp) == Progress::Done);
      exported_instruments.push_back(out);
      exported_slots.push_back(slot);
      // the original of this slot: the file that was read last into it
      const Bytes* original = nullptr;
      for (std::size_t k = 0; k < ptis.size(); ++k) if (ptis[k].name.index == slot) original = &pti_files[k];
      if (original == nullptr) for (std::size_t k = 0; k < mtis.size(); ++k) if (mtis[k].name.index == slot) original = &mti_files[k];
      if (original != nullptr && original->size() >= 392 && (*original)[8] >= 4) {
        ++instrument_files;
        bool same = true;
        for (std::size_t k = 0; k < 388; ++k) {
          if ((*original)[k] != out[k]) {
            if (k >= 60 && k < 64) continue;  // the frame count: this export has one frame
            changed_instrument_bytes.insert(k);
            if (!instrument_free_byte(k)) CHECK_MESSAGE(false, label << ": instrument slot " << slot << " byte " << k);
            if (k >= 4 && k < 16) {  // the header's firmware, version and size bytes: every file of another firmware
              continue;
            }
            same = false;
          }
        }
        instrument_identical += same ? 1u : 0u;
      }
    }

    // ---- read everything exported into the other arena: the same model, byte for byte through the native format
    og_ns::PamdWriter nw;
    Bytes out_names;
    NEED(run_writer(nw, a.project, out_names) == Progress::Done);
    model::ModelArena& b = arena(1);
    NEED(run_reader(project_reader, out_project, b) == Progress::Done);
    for (std::size_t i = 0; i < patterns.size(); ++i) {
      og_ns::PatternWriter tw;
      formats::WriteParams wp;
      wp.index = patterns[i].name.index;
      Bytes out;
      NEED(run_writer(tw, a.project, out, wp) == Progress::Done);
      formats::ReadParams rp;
      rp.index = patterns[i].name.index;
      NEED(run_reader(pattern_reader, out, b, rp) == Progress::Done);
    }
    for (std::size_t i = 0; i < exported_instruments.size(); ++i) {
      formats::ReadParams rp;
      rp.index = static_cast<std::uint16_t>(exported_slots[i]);
      NEED(run_reader(pti_reader, exported_instruments[i], b, rp) == Progress::Done);
      CHECK(pti_reader.import_report().sanitised == 0);
    }
    if (!pamd.empty()) {  // the PAMD file comes back byte for byte (OGPN), reserved bytes and all
      const bool same = out_names == pamd;
      CHECK_MESSAGE(same, label << ": pattern names file");
      c.pamd_identical += same ? 1u : 0u;
    }
    NEED(run_reader(pamd_reader, out_names, b) == Progress::Done);
    // Export always writes a 12-track project: a project of 8 tracks reads back with 12 active ones, which is the
    // one thing that is meant to differ
    CHECK(b.project.set_active_tracks(a.project.active_tracks()) == model::Status::Ok);
    const bool stable = native_bytes(a.project) == native_bytes(b.project);
    CHECK_MESSAGE(stable, label << ": the model after export and import differs from the one imported");
    c.projects_stable += stable ? 1u : 0u;
  }

  std::printf("og fixture survey: %u project folders; projects ok %u err %u; patterns ok %u err %u (8-track %u, 12-track %u); "
              "instruments .mti ok %u, .pti ok %u, skipped %u, err %u; pattern names ok %u err %u; sanitised %u; "
              "crc warnings %u; unverified %u; raw FX ids above 42 %u\n",
              c.groups, c.projects_ok, c.projects_err, c.patterns_ok, c.patterns_err, c.patterns8, c.patterns12,
              c.mti_ok, c.pti_ok, c.instruments_skipped, c.instruments_err, c.pamd_ok, c.pamd_err, c.sanitised,
              c.crc_warn, c.unverified, c.fx_above_42);
  std::printf("og fixture survey: round trip: pattern track blocks identical %u of %u; usage bitmask agrees %u of %u; "
              "instruments identical but for the header bytes %u of %u (%zu distinct byte offsets differ in some file); projects stable %u of %u\n",
              c.pattern_tracks_identical, c.patterns_ok, c.bitmask_agree, c.bitmask_total, instrument_identical,
              instrument_files, changed_instrument_bytes.size(), c.projects_stable, c.projects_ok);

  std::printf("og fixture survey: settings groups identical after export in %u of %u projects (%u hold an OGPM chunk); "
              "pattern names file identical %u of %u\n",
              c.groups_identical, c.projects_ok, c.settings_chunks, c.pamd_identical, c.pamd_ok);
  CHECK(c.groups_identical == c.projects_ok);
  CHECK(c.pamd_identical == c.pamd_ok);
  {
    std::string offs;
    for (std::size_t k : changed_instrument_bytes) offs += std::to_string(k) + " ";
    std::printf("og fixture survey: instrument byte offsets that differ in some file: %s\n", offs.c_str());
  }
  CHECK(c.projects_err == 0);
  CHECK(c.patterns_err == 0);
  CHECK(c.instruments_err == 0);
  CHECK(c.pamd_err == 0);
  CHECK(c.unverified == 0);
  CHECK(c.pattern_tracks_identical == c.patterns_ok);
  CHECK(c.bitmask_agree == c.bitmask_total);
  CHECK(c.projects_stable == c.projects_ok);
  CHECK(c.projects_ok == c.groups);
  CHECK(c.fx_above_42 == 0);  // OS-CEFXIDS risk 6: no fixture pattern holds an OG FX id above 42, so nothing in fixtures-1 changes meaning
}

TEST_CASE("og fixtures: values of the PAR-006 pinned table") {
  const fs::path root = find_fixtures();
  if (root.empty() || !fs::is_directory(root)) return;
  struct Row {
    const char* group_suffix;
    unsigned tempo_x100;
    unsigned song;
    unsigned name_len;
  };
  // PAR-006 section 9.3 (tempo, name length) ; the playlist lengths are 70, 1, 23, 1, 38, 23
  const Row rows[] = {
      {"projver7", 14000, 70, 26}, {"projver11", 9100, 1, 13},  {"projver12", 10500, 23, 9},
      {"projver13", 9200, 1, 10},  {"projver14", 11000, 38, 12}, {"workspace", 10500, 23, 9},
  };
  static og_ns::ProjectReader reader;
  for (const Row& r : rows) {
    fs::path group;
    for (const fs::directory_entry& e : fs::directory_iterator(root)) {
      const std::string n = e.path().filename().string();
      if (n.size() >= std::strlen(r.group_suffix) && n.compare(n.size() - std::strlen(r.group_suffix), std::string::npos, r.group_suffix) == 0) {
        group = e.path();
      }
    }
    if (group.empty()) continue;
    model::ModelArena& a = arena(0);
    NEED(run_reader(reader, read_file(group / "project.mt"), a) == Progress::Done);
    const model::Project& p = a.project;
    CHECK_MESSAGE(p.tempo_x100() == r.tempo_x100, r.group_suffix);
    CHECK_MESSAGE(p.song_length() == r.song, r.group_suffix);
    CHECK_MESSAGE(std::strlen(p.name()) == r.name_len, r.group_suffix);
    CHECK(p.default_pattern_length() == 32);
    CHECK(p.master().reverb_volume == 86);
    CHECK(p.master().dry_volume == 86);
    for (unsigned i = 0; i < 8; ++i) {
      CHECK(p.track(i)->volume >= 74);
      CHECK(p.track(i)->volume <= 90);
    }
    CHECK(p.active_tracks() == (std::string(r.group_suffix) == "workspace" ? 12u : 8u));
  }
}
