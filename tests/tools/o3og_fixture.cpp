// SPDX-License-Identifier: GPL-3.0-or-later
// OS-E2E J2: the OG project of the "OG project migration" journey (sim/scenarios/e2e/e2e_og_migration.txt), written to disk from the synthetic byte maps of the format tests
// (tests/support/og_fixture.h, built from docs/formats/og.md; nothing here comes from a real project or from the official firmware).
//
//   o3og_fixture OUT      writes the card tree into the existing directory OUT:
//     Projects/OG Demo/project.mt                  version 16, "OG Demo", 200 BPM (a step is 75 ms), playlist 1, pattern length 16
//     Projects/OG Demo/patterns/pattern_01.mtp     12 tracks, 16 steps; track 1 holds the notes below, every other track is empty
//     Projects/OG Demo/instruments/01 Sine.pti     slot 0, a loop of 200 frames: two periods of a 441 Hz sine (100 frames a period at 44,100 Hz), plain: a sample, not a wavetable; no envelope, filter or send; volume 40 and panning 60 (not the defaults), so that the journey's probes can catch a dropped field
//
//   o3og_fixture OUT ce   writes a tracker-ce-style v6 project instead (the card ce_v6, sim/scenarios/qa/qa_file_ce.txt, OS-CEPAT S2):
//     Projects/CE Demo/project.mt                  version 16, "CE Demo", 200 BPM, playlist 1, pattern length 16
//     Projects/CE Demo/patterns/pattern_01.mtp     pattern structure v6, 16 tracks of 16 steps; track 1 holds the steps below, every other track is empty
//
// The notes of track 1 (OG note byte 60 is C-5, the pitch the sample was made at): step 1 C-5, step 3 G-5, step 5 C-5, step 7 note off. Step 9 is empty: the journey edits it.
// The folder is generated, never committed (a binary file does not belong in git): sim/CMakeLists.txt, target e2e_og_fixture, runs this into <build>/sim/cards/e2e_og/ on every build;
// tests/e2e_og_fixture_test.py (ctest e2e_fixture_fresh) holds the folder to what this tool writes. Deterministic: integer arithmetic only.

#include "../support/og_fixture.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace og = o3os::test::og;

namespace {

constexpr unsigned kFrames = 200;

// A plain sampler head: the fixture's head with the sample type, the wavetable fields, envelopes, LFOs, filter, glide, sends, overdrive and slices put to nothing, a forward loop over the whole sample;
// volume 40 and panning 60 are the one pair of non-default fields the journey probes after the export and the re-import.
og::Bytes sine_pti() {
  og::InstrumentSpec spec;
  spec.pti = true;
  spec.frames = kFrames;
  spec.file_name = "Sine";
  og::Bytes b = og::instrument_head(spec);
  b[20] = 0;                                         // sample type: a plain sample (the shared head says wavetable)
  og::p16(b, 64, 0);                                 // wavetable window size
  og::p32(b, 88, 0);                                 // current wavetable window
  for (std::size_t i = 92; i < 260; ++i) b[i] = 0;  // six envelopes and six LFOs: none
  og::pf(b, 260, 1.0f);                              // cutoff (open)
  og::pf(b, 264, 0.0f);                              // resonance
  b[268] = 0;
  b[269] = 0;                                        // filter off
  b[270] = 0;
  b[271] = 0;                                        // tune, fine tune
  b[272] = 40;                                       // volume: not the default 50 (unity under the 1.2+ law), so that a re-import which drops the field is caught
  og::p16(b, 274, 0);                                // glide
  og::p16(b, 276, 60);                               // panning: not the centre (50), for the same reason
  b[278] = 0;                                        // delay send
  for (unsigned i = 0; i < 48; ++i) og::p16(b, 280 + 2 * i, 0);
  b[376] = 0;
  b[377] = 0;                                        // slices
  b[384] = 0;                                        // reverb send
  b[385] = 0;                                        // overdrive
  b[386] = 16;                                       // bit depth
  b[76] = 1;                                         // play mode: forward loop
  og::p16(b, 78, 0);                                 // start
  og::p16(b, 80, 0);                                 // loop start
  og::p16(b, 82, 65535);                             // loop end (floor(65535 * 200 / 65535) = 200: the whole sample)
  og::p16(b, 84, 65535);                             // end
  og::p32(b, 60, kFrames);
  og::p32(b, 388, o3os::formats::crc32(b.data(), 388));  // the checksum over the edited head
  // 441 Hz at 44.1 kHz: a period of 100 frames, 12000 at the peak, 16-bit little-endian. The sine is Bhaskara's rational approximation, sin(pi t) = 16 t (1 - t) / (5 - 4 t (1 - t)) per half period
  // (t = k / 50), in integers, so the bytes do not depend on a libm (the error is under 0.2 % of the peak).
  for (unsigned k = 0; k < kFrames; ++k) {
    const std::int32_t phase = static_cast<std::int32_t>(k % 100);
    const std::int32_t x = phase % 50;
    const std::int32_t q = x * (50 - x);
    const std::int32_t mag = 12000 * 16 * q / (12500 - 4 * q);
    const std::int16_t v = static_cast<std::int16_t>(phase < 50 ? mag : -mag);
    b.push_back(static_cast<std::uint8_t>(static_cast<std::uint16_t>(v) & 0xFFu));
    b.push_back(static_cast<std::uint8_t>(static_cast<std::uint16_t>(v) >> 8));
  }
  return b;
}

// OS-CEPAT S2: the v6 project of the card ce_v6 (sim/scenarios/qa/qa_file_ce.txt): a project.mt and one pattern file of structure v6, 16 tracks, built from
// the same byte map. Track 1's steps pin what the v6 reader maps (OS-CEPAT 3.3 and 3.1): a volume column value (step 1), a micromove into a free FX column
// (step 2), a micromove with no free column (step 3, dropped and counted), an OG FX id 75 cleared and 43 mapped to 96 (step 4), a volume above 100 (step 5),
// and the boundaries OG 74 -> 127 and 128 verbatim (step 6).
og::Bytes ce_pattern() {
  og::PatternSpec6 p = og::empty_pattern6(16, 6);
  for (unsigned t = 0; t < 16; ++t) p.length[t] = 15;  // 16 steps
  auto set = [&p](unsigned s, std::uint8_t note, std::uint8_t instr, std::uint8_t t0, std::uint8_t v0, std::uint8_t t1, std::uint8_t v1,
                  std::int8_t vol, std::int8_t m) {
    std::uint8_t* e = p.step[0][s];
    e[0] = note; e[1] = instr;
    e[2] = t0; e[3] = v0; e[4] = t1; e[5] = v1;
    e[6] = static_cast<std::uint8_t>(vol); e[7] = static_cast<std::uint8_t>(m);
  };
  set(1, 60, 0, 0, 0, 0, 0, 50, -1);        // step 1: C-5, volume 50 -> vol 63
  set(2, 62, 0, 0, 0, 0, 0, -1, 50);        // D-5, micromove 50 -> FX 2 value 50 in the first free column
  set(3, 60, 0, 4, 50, 5, 60, -1, 100);     // both columns taken: the micromove is dropped and counted
  set(4, 60, 0, 75, 10, 43, 11, -1, -1);    // OG 75 cleared, OG 43 -> model 96
  set(5, 60, 0, 0, 0, 0, 0, 101, -1);       // volume above 100: full volume, counted
  set(6, 60, 0, 74, 9, 128, 200, -1, -1);   // the boundaries: 74 -> 127, 128 verbatim, neither counted
  return og::pattern6_bytes(p);
}

bool put(const std::filesystem::path& root, const std::string& rel, const og::Bytes& bytes) {
  const std::filesystem::path path = root / rel;
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return !ec && f.good();
}

}  // namespace

int main(int argc, char** argv) {
  const bool ce = argc == 3 && std::strcmp(argv[2], "ce") == 0;
  if (argc != 2 && !ce) {
    std::fprintf(stderr, "usage: o3og_fixture OUT [ce]   (the directory must exist; 'ce' writes the tracker-ce v6 project instead)\n");
    return 2;
  }
  const std::filesystem::path root(argv[1]);
  if (!std::filesystem::is_directory(root)) {
    std::fprintf(stderr, "o3og_fixture: %s is not a directory\n", argv[1]);
    return 2;
  }
  if (ce) {
    og::ProjectSpec ps;
    ps.name = "CE Demo";
    ps.tempo = 200.0f;
    ps.pattern_length = 16;
    ps.playlist = {1};
    ps.bitmask[0] = 1;  // pattern 1 is in the song
    const std::string folder = "Projects/CE Demo/";
    const bool ok = put(root, folder + "project.mt", og::project_bytes(ps)) && put(root, folder + "patterns/pattern_01.mtp", ce_pattern());
    if (!ok) {
      std::fprintf(stderr, "o3og_fixture: cannot write under %s\n", argv[1]);
      return 1;
    }
    return 0;
  }
  og::ProjectSpec ps;
  ps.name = "OG Demo";
  ps.tempo = 200.0f;
  ps.pattern_length = 16;
  ps.playlist = {1};
  ps.mute[1] = 0;
  ps.mute[6] = 0;
  og::PatternSpec pat = og::empty_pattern(12, 5);
  for (unsigned t = 0; t < 12; ++t) pat.length[t] = 15;  // 16 steps
  auto note = [&pat](unsigned step, int n, unsigned instrument_byte) {
    pat.step[0][step][0] = static_cast<std::uint8_t>(n);
    pat.step[0][step][1] = static_cast<std::uint8_t>(instrument_byte);
  };
  note(1, 60, 0);
  note(3, 67, 0);
  note(5, 60, 0);
  note(7, -4, 0);  // off
  ps.bitmask[0] = og::usage_byte(pat);
  const std::string folder = "Projects/OG Demo/";
  const bool ok = put(root, folder + "project.mt", og::project_bytes(ps)) && put(root, folder + "patterns/pattern_01.mtp", og::pattern_bytes(pat)) &&
                  put(root, folder + "instruments/01 Sine.pti", sine_pti());
  if (!ok) {
    std::fprintf(stderr, "o3og_fixture: cannot write under %s\n", argv[1]);
    return 1;
  }
  return 0;
}
