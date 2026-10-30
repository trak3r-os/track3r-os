// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-007g: "Play after a project load makes no sound", end to end and offline. The unit's sequence, through the same libraries and in the same order as the MK66
// composition root (platform/mk66/src/app_main.cpp), minus the hardware: the fresh document with the demo kit and its pattern, the sequencer over the project as the
// engine's event source, Play and Stop; then a project load (the Stop and the settle, the OG readers into the document, the voices' table cleared, the sample
// loader into the arena, the demo kit again only for a project with no sample instrument) with the audio blocks running on through it, then Play again. The output
// of the second Play must not be silent, and the notes must reach the voices and the master meters.
//
// Two projects, each loaded from a host directory (DirStorage) and from a FAT32 volume read through FatFs (FatStorage, the device's card stack minus the SD
// protocol): a small one built here (one sample instrument, one note: runs in CI), and a v1.9.0 Workspace from the optional local fixtures/ directory (skipped when it is absent; nothing of
// it is printed, only counts). Host only (it makes files, and the document is 1.9 MB).
//
// OS-007j, "the drums and some other channels are missing" on the unit: a track the project saved MUTED (the project's track mute flags, 0x12C) is silent, and
// that was all of it (the fixture mutes four tracks; the voices, the slice mapping and the graph were sound). The last tests hold that down per track: a muted
// track starts no note and sounds when it is unmuted, and in the fixture every track that has notes sounds unless the project muted it.
#include <doctest/doctest.h>

#include "support/need.h"
#include "support/og_folder_fixture.h"
#include "support/static_slot.h"

#include "../platform/mk66/tests/mk66_storage_fakes.h"

#include <o3os/app/og_folder.h>
#include <o3os/app/og_samples.h>
#include <o3os/app/project_state.h>
#include <o3os/app/sample_bank.h>
#include <o3os/audio/demo_kit.h>
#include <o3os/audio/engine.h>
#include <o3os/audio/meters.h>
#include <o3os/audio/voices.h>
#include <o3os/hal/audio.h>
#include <o3os/hal/storage.h>
#include <o3os/model/demo.h>
#include <o3os/model/project.h>
#include <o3os/platform/host/dir_storage.h>
#include <o3os/platform/mk66/storage/fat_storage.h>
#include <o3os/seq/engine_link.h>
#include <o3os/seq/sequencer.h>

#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace haudio = o3os::hal::audio;
namespace hs = o3os::hal::storage;
namespace st = o3os::platform::mk66::storage;
using o3os::platform::host::DirStorage;
using namespace o3os;
using namespace o3os::test::og;

namespace {

constexpr std::uint32_t kBlk = haudio::kBlock;
constexpr std::uint32_t kArenaSamples = 5400000;  // the device's 10.8 MB sample arena (mem_map.h)
constexpr std::uint32_t kOneSecond = 44100u / kBlk;
constexpr std::uint32_t kTwoSeconds = 2u * kOneSecond;

std::uint32_t fake_cycles_counter = 0;
std::uint32_t fake_cycles() noexcept { return fake_cycles_counter += 11u; }

// ---- the projects, as the files of one folder -----------------------------------------------------------------------------------------

// A mono 16-bit 44.1 kHz WAV of `frames` frames: a square wave of 441 Hz at +-12000.
Bytes wav_bytes(std::uint32_t frames) {
  Bytes b(44 + 2u * frames, 0);
  std::memcpy(b.data(), "RIFF", 4);
  p32(b, 4, 36u + 2u * frames);
  std::memcpy(b.data() + 8, "WAVEfmt ", 8);
  p32(b, 16, 16);
  p16(b, 20, 1);
  p16(b, 22, 1);
  p32(b, 24, 44100);
  p32(b, 28, 88200);
  p16(b, 32, 2);
  p16(b, 34, 16);
  std::memcpy(b.data() + 36, "data", 4);
  p32(b, 40, 2u * frames);
  for (std::uint32_t k = 0; k < frames; ++k) {
    p16(b, 44 + 2u * k, static_cast<std::uint16_t>(static_cast<std::int16_t>(((k / 50u) & 1u) != 0u ? 12000 : -12000)));
  }
  return b;
}

// The small project: one sample instrument in slot 0 (instruments/instrument_01.mti and samples/instr01.wav), pattern 1 with a note on track 1 on every fourth
// step (the OG's instrument byte 0 is slot 0), 16 steps, a song of pattern 1.
std::vector<FolderFile> synthetic_files() {
  std::vector<FolderFile> files;
  ProjectSpec ps;
  ps.version = 16;
  ps.tempo = 120.0f;
  ps.pattern_length = 16;
  ps.playlist = {1};
  files.push_back({"project.mt", project_bytes(ps)});
  PatternSpec pat = empty_pattern(12, 5);
  for (unsigned t = 0; t < 12; ++t) {
    pat.length[t] = 15;  // the last step index: 16 steps
  }
  for (unsigned s = 0; s < 16; s += 4) {
    pat.step[0][s][0] = 60;  // note 60 (C-5), instrument 0
    pat.step[0][s][1] = 0;
  }
  files.push_back({"patterns/pattern_01.mtp", pattern_bytes(pat)});
  InstrumentSpec mti;
  mti.pti = false;
  mti.file_name = "Tone";
  files.push_back({"instruments/instrument_01.mti", instrument_bytes(mti)});
  files.push_back({"samples/instr01.wav", wav_bytes(20000)});
  return files;
}

// Every file under `dir`, with its path relative to it ('/' separated). Read by path, never printed.
std::vector<FolderFile> files_of(const fs::path& dir) {
  std::vector<FolderFile> files;
  for (const fs::directory_entry& e : fs::recursive_directory_iterator(dir)) {
    if (!e.is_regular_file()) {
      continue;
    }
    std::ifstream in(e.path(), std::ios::binary);
    files.push_back({fs::relative(e.path(), dir).generic_string(), Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>())});
  }
  return files;
}

// The fixture's `sd` directory, or empty: O3_FIXTURES_DIR, else `fixtures/sd` of the nearest ancestor of this file that has one (a build tree may sit inside another checkout).
fs::path find_fixtures() {
  if (const char* env = std::getenv("O3_FIXTURES_DIR")) {
    return fs::path(env);
  }
  fs::path p = fs::absolute(fs::path(__FILE__)).parent_path();
  while (true) {
    if (fs::is_directory(p / "fixtures" / "sd")) {
      return p / "fixtures" / "sd";
    }
    if (!p.has_parent_path() || p == p.parent_path()) {
      return {};
    }
    p = p.parent_path();
  }
}

struct TempDir {
  fs::path path;
  TempDir() {
    static unsigned counter = 0;
    path = fs::temp_directory_path() / ("o3os_play_after_load_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
};

void write_dir(const fs::path& root, const std::string& folder, const std::vector<FolderFile>& files) {
  for (const FolderFile& f : files) {
    const fs::path p = root / folder / f.path;
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(f.bytes.data()), static_cast<std::streamsize>(f.bytes.size()));
  }
}

// `files` under `folder` of a FAT32 volume (each parent directory made once, in the order they first appear).
void write_fat(fakes::Fat32Builder& fat, const std::string& folder, const std::vector<FolderFile>& files) {
  std::map<std::string, std::uint32_t> dirs;
  dirs[""] = fakes::Fat32Builder::kRootCluster;
  const auto dir_of = [&](const std::string& path) {
    std::string so_far;
    std::uint32_t cluster = fakes::Fat32Builder::kRootCluster;
    std::size_t at = 0;
    while (at < path.size()) {
      std::size_t slash = path.find('/', at);
      if (slash == std::string::npos) {
        slash = path.size();
      }
      so_far += (so_far.empty() ? "" : "/") + path.substr(at, slash - at);
      const auto it = dirs.find(so_far);
      if (it == dirs.end()) {
        cluster = fat.make_dir(cluster, path.substr(at, slash - at));
        dirs[so_far] = cluster;
      } else {
        cluster = it->second;
      }
      at = slash + 1;
    }
    return cluster;
  };
  for (const FolderFile& f : files) {
    const std::string full = folder + "/" + f.path;
    const std::size_t slash = full.rfind('/');
    fat.add_file(dir_of(full.substr(0, slash)), full.substr(slash + 1), f.bytes);
  }
}

// ---- the device ---------------------------------------------------------------------------------------------------------------------

// The composition root's sound and load, as app_main.cpp builds and runs them. Statics: the document, the engine and the sequencer do not fit a stack.
struct Device {
  model::ModelArena* arena = nullptr;
  audio::VoicePool* voices = nullptr;
  audio::Engine* engine = nullptr;
  seq::Sequencer* seq = nullptr;
  std::vector<std::int16_t> sample_arena = std::vector<std::int16_t>(kArenaSamples, std::int16_t{0});
  audio::SeqEvent events[audio::kMaxEventsPerBlock];
  alignas(8) std::int16_t in[haudio::kBlockSamples] = {};
  alignas(8) std::int16_t out[haudio::kBlockSamples] = {};
  std::int32_t peak = 0;  // of the output, since reset_peak()

  // install_demo()
  void install_demo() {
    if (voices->has_samples()) {
      return;
    }
    audio::DemoKit kit;
    if (audio::generate_demo_kit(sample_arena.data(), kArenaSamples, kit)) {
      for (unsigned i = 0; i < audio::kDemoKitSounds; ++i) {
        voices->set_sample(i, kit.sample[i]);
      }
      (void)model::demo::add_instruments(arena->project);
      if (model::demo::pattern_is_empty(arena->project, 1)) {
        (void)model::demo::fill_pattern(arena->project, 1);
      }
    }
  }

  // make_document() + make_sound()
  void boot() {
    static model::ModelArena document;
    static test::StaticSlot<audio::VoicePool> pool_slot;
    static test::StaticSlot<audio::Engine> engine_slot;
    static test::StaticSlot<seq::Sequencer> seq_slot;
    static audio::TapMem taps;
    arena = &document;
    model::Project& project = arena->project;
    project.reset();
    (void)project.create_pattern(1, model::kDefaultPatternLength);
    (void)project.append_order(1);
    voices = &pool_slot.make();
    engine = &engine_slot.make(&fake_cycles, taps);
    if (project.instrument_count() == 0) {
      install_demo();
    }
    seq = &seq_slot.make(project);
    engine->attach_voices(voices);
    engine->attach_events(seq::event_source(*seq, events, audio::kMaxEventsPerBlock));
    peak = 0;
  }

  void block() {
    haudio::BlockReport report;
    const haudio::BlockIo io{in, out, 0, &report};
    engine->render_block(io);
    for (std::uint32_t i = 0; i < haudio::kBlockSamples; ++i) {
      const std::int32_t a = out[i] < 0 ? -static_cast<std::int32_t>(out[i]) : static_cast<std::int32_t>(out[i]);
      peak = a > peak ? a : peak;
    }
  }
  void render(std::uint32_t blocks) {
    for (std::uint32_t b = 0; b < blocks; ++b) {
      block();
    }
  }
  void reset_peak() { peak = 0; }

  // What App::document_replaced does after a load (OS-MUTE): the strips take the project's mute and solo. The sequencer no longer skips a muted track.
  void apply_tracks() { (void)app::tracks_apply(arena->project, &engine->params()); }

  // Play and Stop on the sequencer's ring, as SeqTransport does with the outputs open.
  bool play(unsigned pattern) { return seq->commands().push(seq::Command{seq::Command::Kind::PlayPattern, static_cast<std::uint8_t>(pattern), 0, 0}); }
  bool stop() { return seq->commands().push(seq::Command{seq::Command::Kind::Stop, 0, 0, 0}); }

  // start_project_load() .. finish_samples(): the Stop, the settle (blocks until the voices are quiet), the project, the table cleared, the samples, the demo kit for a
  // project the kit fits (app::demo_kit_fits, OS-BUG-DEMOKIT: no sample or module instrument, nothing in slots 1-4). The audio blocks run on through it, about one for each slice of the loaders. False: the project or its samples did not load.
  bool load(hs::Storage& card, const char* folder, app::OgFolderLoader& loader, app::OgSampleLoader& samples) {
    (void)stop();
    for (unsigned i = 0; i < 100u && (i < 3u || voices->voices_in_use() != 0u); ++i) {
      block();
    }
    if (!loader.begin(card, folder, *arena)) {
      return false;
    }
    for (unsigned n = 0; n < 200000u && loader.state() == app::OgFolderLoader::State::Running; ++n) {
      (void)loader.step(16 * 1024);
      block();
    }
    if (loader.state() != app::OgFolderLoader::State::Done) {
      return false;
    }
    for (unsigned i = 0; i < audio::kSampleSlots; ++i) {
      voices->set_sample(i, audio::SampleDesc{});
    }
    if (!samples.begin(card, loader.folder(), arena->project, loader.pti_slots(), *voices, sample_arena.data(), kArenaSamples)) {
      return false;
    }
    for (unsigned n = 0; n < 200000u && samples.state() == app::OgSampleLoader::State::Running; ++n) {
      (void)samples.step(16 * 1024);
      block();
    }
    if (app::demo_kit_fits(arena->project)) {
      install_demo();
    }
    return true;
  }
};

Device& device() {
  static Device d;
  return d;
}

app::OgFolderLoader& folder_loader() {
  static app::OgFolderLoader l;
  return l;
}

app::OgSampleLoader& sample_loader() {
  static app::OgSampleLoader l;
  return l;
}

// The whole story on `card`: the demo plays, (optionally Stop), load `folder`, Play pattern 1, then Stop and Play again.
void run_story(hs::Storage& card, const char* folder, bool stop_before_load) {
  Device& d = device();
  d.boot();

  // 1. The demo plays (the unit's boot).
  NEED(d.play(1));
  d.render(kTwoSeconds);
  CHECK_MESSAGE(d.peak > 8000, "the demo does not sound before the load: peak " << d.peak);
  if (stop_before_load) {
    NEED(d.stop());
    d.render(60);
    CHECK_FALSE(d.seq->playing());
  }  // else the load's own Stop has to stop it, as `diag load` does

  // 2. The load.
  NEED(d.load(card, folder, folder_loader(), sample_loader()));
  CHECK(sample_loader().report().loaded >= 1u);
  CHECK(sample_loader().report().storage_error == hs::Status::Ok);
  CHECK(d.voices->has_samples());
  CHECK_FALSE(d.seq->playing());

  // 3. Play: sound, from the first pass of the pattern on.
  d.reset_peak();
  const std::uint32_t note_ons = d.voices->stats().note_ons;
  const std::uint32_t ignored = d.voices->stats().ignored;
  NEED(d.play(1));
  d.render(kTwoSeconds);
  CHECK_MESSAGE(d.seq->playing(), "the sequencer is not playing after the Play that follows the load");
  CHECK_MESSAGE(d.voices->stats().note_ons > note_ons, "no note reached a voice after the load (ignored " << d.voices->stats().ignored - ignored << ", rows missing "
                                                       << d.seq->stats().rows_missing << ", torn " << d.seq->stats().rows_torn << ")");
  CHECK(d.voices->stats().ignored == ignored);  // every note had a sample to play
  CHECK(d.seq->stats().rows_missing == 0u);
  CHECK(d.seq->stats().rows_torn == 0u);
  CHECK_MESSAGE(d.peak > 500, "the output is silent after the load: peak " << d.peak);
  audio::MeterSnapshot snap;
  CHECK(d.engine->graph().meters().snapshot(snap));
  CHECK_MESSAGE(snap.ch[audio::meter_ch::kMasterL].hold != 0u, "the master meter shows nothing after the load");
  CHECK(snap.ch[audio::meter_ch::kMainL].hold != 0u);

  // 4. Stop and Play again, twice: the same.
  for (unsigned again = 0; again < 2; ++again) {
    NEED(d.stop());
    d.render(again == 0 ? 0u : 20u);
    NEED(d.play(1));
    d.reset_peak();
    d.render(kOneSecond);
    CHECK_MESSAGE(d.peak > 500, "silent on Play number " << again + 2 << " after the load: peak " << d.peak);
  }
}

}  // namespace

TEST_CASE("play after load: a small project (one sample instrument, one note) on a host directory sounds after the demo has played and the project is loaded") {
  TempDir tmp;
  write_dir(tmp.path, "Tiny", synthetic_files());
  DirStorage card(tmp.path.string());
  NEED(card.mount() == hs::Status::Ok);
  run_story(card, "Tiny", true);
  run_story(card, "Tiny", false);
}

TEST_CASE("play after load: the same small project on a FAT32 volume read through FatFs") {
  fakes::Medium medium(200'000);
  {
    fakes::Fat32Builder fat(medium);
    write_fat(fat, "Projects/Tiny", synthetic_files());
  }
  st::FatStorage card(medium);
  NEED(card.mount() == hs::Status::Ok);
  run_story(card, "Projects/Tiny", true);
  run_story(card, "Projects/Tiny", false);
}

TEST_CASE("play after load: a v1.9.0 Workspace (fixtures, skipped when absent) on a host directory and on FAT32") {
  const fs::path root = find_fixtures();
  if (root.empty() || !fs::is_directory(root / "v1.9.0-workspace")) {
    std::printf("play after load: no fixtures found, skipped\n");
    return;
  }
  {
    DirStorage card(root.string());
    NEED(card.mount() == hs::Status::Ok);
    run_story(card, "v1.9.0-workspace", true);
    run_story(card, "v1.9.0-workspace", false);
  }
  {
    const std::vector<FolderFile> files = files_of(root / "v1.9.0-workspace");
    NEED(files.size() > 10u);
    fakes::Medium medium(200'000);
    {
      fakes::Fat32Builder fat(medium, 80'000);
      write_fat(fat, "Projects/t-v16", files);
    }
    st::FatStorage card(medium);
    NEED(card.mount() == hs::Status::Ok);
    run_story(card, "Projects/t-v16", false);
  }
}

// ---- OS-007j: which tracks sound -------------------------------------------------------------------------------------------------------

namespace {

// The peak of every track's voice block (what the graph takes as the strip's input) and of its strip (the meter after the fader) over `blocks` blocks of the pattern playing.
struct TrackPeaks {
  std::int32_t peak[audio::kVoiceTracks] = {};
  std::uint32_t strip[audio::kVoiceTracks] = {};
};

TrackPeaks play_pattern_peaks(Device& d, unsigned pattern, std::uint32_t blocks) {
  TrackPeaks r;
  (void)d.stop();
  d.render(20);
  (void)d.play(pattern);
  for (std::uint32_t b = 0; b < blocks; ++b) {
    d.block();
    for (unsigned t = 0; t < audio::kVoiceTracks; ++t) {
      const std::int16_t* f = d.voices->feed(t);
      for (std::uint32_t k = 0; f != nullptr && k < kBlk; ++k) {
        const std::int32_t a = f[k] < 0 ? -static_cast<std::int32_t>(f[k]) : static_cast<std::int32_t>(f[k]);
        r.peak[t] = a > r.peak[t] ? a : r.peak[t];
      }
    }
    audio::MeterSnapshot snap;
    if (d.engine->graph().meters().snapshot(snap)) {
      for (unsigned t = 0; t < audio::kVoiceTracks; ++t) {
        r.strip[t] = snap.ch[audio::meter_ch::kTrack0 + t].peak > r.strip[t] ? snap.ch[audio::meter_ch::kTrack0 + t].peak : r.strip[t];
      }
    }
  }
  return r;
}

// Three tracks with a note on every fourth step of pattern 1 (the instrument byte 0: the sample of slot 0), track 2 (index 1) saved muted, the project saved on
// pattern 2 (an empty one), a song of 1 and 2.
std::vector<FolderFile> muted_files() {
  std::vector<FolderFile> files;
  ProjectSpec ps;
  ps.version = 16;
  ps.tempo = 120.0f;
  ps.pattern_length = 16;
  ps.playlist = {1, 2};
  ps.actual_pattern = 2;
  for (std::uint8_t& m : ps.mute) {
    m = 0;
  }
  ps.mute[1] = 1;
  files.push_back({"project.mt", project_bytes(ps)});
  PatternSpec one = empty_pattern(12, 5);
  PatternSpec two = empty_pattern(12, 5);
  for (unsigned t = 0; t < 12; ++t) {
    one.length[t] = 15;
    two.length[t] = 15;
  }
  for (unsigned t = 0; t < 3; ++t) {
    for (unsigned s = 0; s < 16; s += 4) {
      one.step[t][s][0] = 60;
      one.step[t][s][1] = 0;
    }
  }
  files.push_back({"patterns/pattern_01.mtp", pattern_bytes(one)});
  files.push_back({"patterns/pattern_02.mtp", pattern_bytes(two)});
  InstrumentSpec mti;
  mti.pti = false;
  mti.file_name = "Tone";
  files.push_back({"instruments/instrument_01.mti", instrument_bytes(mti)});
  files.push_back({"samples/instr01.wav", wav_bytes(20000)});
  return files;
}

}  // namespace

TEST_CASE("play tracks: a track the project saved muted keeps playing and is silent at its strip; unmuted it is heard (OS-MUTE, was OS-007j)") {
  TempDir tmp;
  write_dir(tmp.path, "Muted", muted_files());
  DirStorage card(tmp.path.string());
  NEED(card.mount() == hs::Status::Ok);
  Device& d = device();
  d.boot();
  NEED(d.load(card, "Muted", folder_loader(), sample_loader()));
  // The mute is the project's: the loader read it, and the report says which pattern the project opens on.
  const model::TrackSlot* slot1 = d.arena->project.track(1);
  NEED(slot1 != nullptr);
  CHECK((slot1->flags & model::kTrackMute) != 0u);
  CHECK(folder_loader().report().start_pattern == 2u);
  d.apply_tracks();  // what the App does when the document is replaced: the strips take the project's mute

  const std::uint32_t blocks = 2u * kOneSecond;
  const std::uint32_t before = d.voices->stats().note_ons;
  TrackPeaks run = play_pattern_peaks(d, 1, blocks);
  CHECK_MESSAGE(run.peak[0] > 500, "track 1 is silent: peak " << run.peak[0]);
  // OS-ODD11: the muted track's voice plays its notes but is gated once its strip has closed (no post stages, no block for the track): its note-ons are counted below, its gated blocks here.
  CHECK_MESSAGE(d.voices->stats().gated_blocks > 0u, "the muted track 2's voice is never gated");
  CHECK_MESSAGE(run.peak[2] > 500, "track 3 is silent: peak " << run.peak[2]);
  CHECK_MESSAGE(run.strip[0] > 500, "track 1's strip is silent: " << run.strip[0]);
  CHECK_MESSAGE(run.strip[1] == 0u, "the muted track 2 is heard at its strip: " << run.strip[1]);
  CHECK_MESSAGE(run.strip[2] > 500, "track 3's strip is silent: " << run.strip[2]);
  const std::uint32_t three_tracks = d.voices->stats().note_ons - before;  // the muted track makes its notes: all three tracks reach the voices
  CHECK(three_tracks > 0u);

  // Unmuted (the pad's Shift + 2: the Track parameter), the same pattern is heard on all three tracks.
  model::TrackSlot unmuted = *slot1;
  unmuted.flags = 0;
  NEED(d.arena->project.set_track(1, unmuted) == model::Status::Ok);
  d.apply_tracks();
  run = play_pattern_peaks(d, 1, blocks);
  CHECK_MESSAGE(run.strip[1] > 500, "track 2 is silent at its strip after the unmute: " << run.strip[1]);
}

TEST_CASE("play tracks: a v1.9.0 Workspace (fixtures, skipped when absent): every track that has notes sounds unless the project muted it (OS-007j)") {
  const fs::path root = find_fixtures();
  if (root.empty() || !fs::is_directory(root / "v1.9.0-workspace")) {
    std::printf("play tracks: no fixtures found, skipped\n");
    return;
  }
  DirStorage card(root.string());
  NEED(card.mount() == hs::Status::Ok);
  Device& d = device();
  d.boot();
  NEED(d.load(card, "v1.9.0-workspace", folder_loader(), sample_loader()));
  d.apply_tracks();
  const model::Project& project = d.arena->project;
  CHECK(folder_loader().report().start_pattern >= 1u);

  unsigned with_notes = 0;
  unsigned muted_silent = 0;
  unsigned unmuted_silent = 0;
  for (unsigned id = 1; id <= model::kMaxPatternId; ++id) {
    const model::Pattern* p = project.pattern(id);
    if (p == nullptr) {
      continue;
    }
    // The tracks that have a note whose instrument has a sample (a note on an empty slot is ignored, counted, and silent whoever mutes it).
    bool has[audio::kVoiceTracks] = {};
    bool any = false;
    for (unsigned t = 0; t < audio::kVoiceTracks; ++t) {
      for (unsigned s = 0; s < p->track_length[t]; ++s) {
        const model::Step& st = p->steps[t][s];
        if (st.note >= model::kNoteFirst && st.note <= model::kNoteLast && st.instr >= 1u && st.instr <= audio::kSampleSlots && d.voices->sample(st.instr - 1u).length != 0u) {
          has[t] = true;
          any = true;
          break;
        }
      }
    }
    if (!any) {
      continue;
    }
    const std::uint64_t blocks = static_cast<std::uint64_t>(project.pattern_length(id)) * 15u * 44100u * 100u / project.tempo_x100() / kBlk + 100u;  // one pass
    const TrackPeaks run = play_pattern_peaks(d, id, static_cast<std::uint32_t>(blocks));
    for (unsigned t = 0; t < audio::kVoiceTracks; ++t) {
      if (!has[t]) {
        continue;
      }
      ++with_notes;
      const model::TrackSlot* slot = project.track(t);
      const bool muted = slot != nullptr && (slot->flags & model::kTrackMute) != 0u;
      if (muted) {
        ++muted_silent;
        CHECK_MESSAGE(run.strip[t] == 0u, "pattern " << id << " track " << t + 1 << " is muted by the project and is heard at its strip: peak " << run.strip[t]);
      } else if (run.peak[t] == 0) {
        ++unmuted_silent;
        MESSAGE("pattern " << id << " track " << t + 1 << " has notes, is not muted, and is silent");
      }
    }
  }
  std::printf("play tracks: %u (pattern, track) pairs with notes, %u of them muted by the project, %u unmuted and silent\n", with_notes, muted_silent, unmuted_silent);
  CHECK(with_notes > 0u);
  CHECK(unmuted_silent == 0u);
}
