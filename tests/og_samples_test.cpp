// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-007f: the sample PCM of a loaded OG project goes into the arena and is bound to the voices (app/og_samples.h). A small project folder is built in a
// temporary directory and read through the host storage backend (DirStorage), as og_folder_test.cpp does: three sample instruments, one a `.mti` whose
// samples/instr01.wav is there (with a forward loop, an inner window, tune and volume), one a `.mti` whose WAV is missing, one a `.pti` (the PCM after its
// 392-byte head: the form of a .pti in an OG Workspace). The loader is stepped in small slices, as the main loop does, and what the voices were given is
// compared with the reference arithmetic. A second case gives it an arena that holds the .pti and not the WAV. Host only (it makes files).
#include <doctest/doctest.h>

#include "support/iff.h"
#include "support/need.h"
#include "support/og_folder_check.h"
#include "support/project_fixture.h"
#include "support/static_slot.h"

#include <o3os/app/og_folder.h>
#include <o3os/app/og_samples.h>
#include <o3os/app/sample_bank.h>
#include <o3os/audio/voices.h>
#include <o3os/core/audio_clock.h>
#include <o3os/formats/crc32.h>
#include <o3os/hal/storage.h>
#include <o3os/model/demo.h>
#include <o3os/model/project.h>
#include <o3os/platform/host/dir_storage.h>

#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace hs = o3os::hal::storage;
using o3os::app::OgFolderLoader;
using o3os::app::OgSampleLoader;
using o3os::app::SampleProblem;
using o3os::audio::SampleDesc;
using o3os::audio::VoicePool;
using o3os::platform::host::DirStorage;
using namespace o3os::test::og;

namespace {

// The envelope time of `ms` milliseconds in groups of 8 samples, from the clock's rational (truncated samples, rounded up to a group, 1..65535): the reference the unit is checked against.
std::uint64_t ref_groups(unsigned ms) {
  const std::uint64_t samples = static_cast<std::uint64_t>(ms) * (o3os::core::kCpuHz / 1000u) * o3os::core::kRefCyclesPerSample.den / o3os::core::kRefCyclesPerSample.num;
  const std::uint64_t units = (samples + 7u) / 8u;
  return units < 1u ? 1u : (units > 65535u ? 65535u : units);
}

struct TempDir {
  fs::path path;
  TempDir() {
    static unsigned counter = 0;
    path = fs::temp_directory_path() / ("o3os_og_samples_test_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
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

void write_file(const fs::path& root, const std::string& rel, const Bytes& bytes) {
  const fs::path p = root / rel;
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::map<std::string, std::string> snapshot(const fs::path& root) {
  std::map<std::string, std::string> out;
  for (const fs::directory_entry& e : fs::recursive_directory_iterator(root)) {
    const std::string rel = fs::relative(e.path(), root).generic_string();
    if (e.is_directory()) {
      out[rel + "/"] = "";
    } else {
      std::ifstream in(e.path(), std::ios::binary);
      out[rel] = std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
  }
  return out;
}

// A canonical OG sample file: 44 bytes of head, mono 16-bit 44.1 kHz, the sample k being the int16 pcm_at(k).
std::int16_t pcm_at(std::uint32_t k) { return static_cast<std::int16_t>(k * 7u + 3u); }

Bytes wav_bytes(std::uint32_t frames) {
  Bytes b(44 + 2u * frames, 0);
  std::memcpy(b.data(), "RIFF", 4);
  p32(b, 4, 36u + 2u * frames);
  std::memcpy(b.data() + 8, "WAVEfmt ", 8);
  p32(b, 16, 16);
  p16(b, 20, 1);      // PCM
  p16(b, 22, 1);      // mono
  p32(b, 24, 44100);
  p32(b, 28, 88200);
  p16(b, 32, 2);
  p16(b, 34, 16);
  std::memcpy(b.data() + 36, "data", 4);
  p32(b, 40, 2u * frames);
  for (std::uint32_t k = 0; k < frames; ++k) {
    p16(b, 44 + 2u * k, static_cast<std::uint16_t>(pcm_at(k)));
  }
  return b;
}

// An instrument file of the fixture with its sampler fields set (the offsets of docs/formats/og.md section 5) and its CRC made again.
Bytes instrument_file(const InstrumentSpec& spec, std::uint8_t mode, std::uint16_t start, std::uint16_t loop1, std::uint16_t loop2, std::uint16_t end,
                      std::int8_t tune, std::int8_t fine, std::uint8_t volume) {
  Bytes b = instrument_bytes(spec);
  b[76] = mode;
  p16(b, 78, start);
  p16(b, 80, loop1);
  p16(b, 82, loop2);
  p16(b, 84, end);
  b[270] = static_cast<std::uint8_t>(tune);
  b[271] = static_cast<std::uint8_t>(fine);
  b[272] = volume;
  p32(b, 388, o3os::formats::crc32(b.data(), 388));
  return b;
}

constexpr std::uint32_t kWavFrames = 65535;  // so that a point 0..65535 is the sample position itself (floor(point * 65535 / 65535) is exact)
constexpr std::uint32_t kPtiFrames = 1000;

void write_project(const fs::path& root) {
  write_file(root, "Workspace/project.mt", project_bytes(ProjectSpec{}));
  InstrumentSpec mti;
  mti.pti = false;
  mti.file_name = "Looped";
  // slot 0: a forward loop inside the window [1000, 60000), points as positions in the sample
  write_file(root, "Workspace/instruments/instrument_01.mti", instrument_file(mti, 1, 1000, 20000, 40000, 60000, 3, -20, 80));
  write_file(root, "Workspace/samples/instr01.wav", wav_bytes(kWavFrames));
  // slot 1: its WAV is not on the card
  mti.file_name = "Gone";
  write_file(root, "Workspace/instruments/instrument_02.mti", instrument_file(mti, 0, 0, 0, 65535, 65535, 0, 0, 100));
  // slot 2: a .pti, the whole sample, a play mode the voices do not have (granular)
  InstrumentSpec pti;
  pti.pti = true;
  pti.frames = kPtiFrames;
  pti.file_name = "Hat";
  write_file(root, "Workspace/instruments/instrument_03.pti", instrument_file(pti, 7, 0, 0, 65535, 65535, -24, 100, 100));
}

// Loads the project, then its samples into `arena`, in slices of `budget` bytes; `steps` is the number of steps the sample loader took. False: the project did not load.
bool load_all(DirStorage& card, std::vector<std::int16_t>& arena, VoicePool& pool, OgSampleLoader& sl, OgFolderLoader& pl, o3os::model::ModelArena& model,
              std::size_t budget, unsigned& steps) {
  model.project.reset();
  if (!pl.begin(card, "Workspace", model) || run_loader(pl, 4096) != OgFolderLoader::State::Done) {
    return false;
  }
  // as the app does (OS-007i): the instrument volume law is the project file's (the fixture's is version 16: the 1.2+ law)
  if (!sl.begin(card, pl.folder(), model.project, pl.pti_slots(), pool, arena.data(), static_cast<std::uint32_t>(arena.size()),
                o3os::app::instrument_law(pl.report().version))) {
    return false;
  }
  steps = 0;
  while (sl.state() == OgSampleLoader::State::Running && steps < 1000000u) {
    (void)sl.step(budget);
    ++steps;
  }
  return true;
}

VoicePool& pool() {
  static o3os::test::StaticSlot<VoicePool> slot;
  return slot.make();
}

OgSampleLoader& sample_loader() {
  static OgSampleLoader sl;
  return sl;
}

}  // namespace

TEST_CASE("og samples: a .mti with its WAV, one without, and a .pti are bound as the OG's points say; nothing on the card is written") {
  TempDir tmp;
  write_project(tmp.path);
  const std::map<std::string, std::string> before = snapshot(tmp.path);
  DirStorage card(tmp.path.string());
  NEED(card.mount() == hs::Status::Ok);
  std::vector<std::int16_t> arena(100000, std::int16_t{-1});
  VoicePool& p = pool();
  OgSampleLoader& sl = sample_loader();
  o3os::model::ModelArena& model = o3os::test::og::arena(0);

  unsigned steps = 0;
  NEED(load_all(card, arena, p, sl, loader(), model, 600, steps));
  CHECK(steps > 100u);  // 131 KB of WAV in slices of 600 bytes: many passes of the main loop, not one
  NEED(sl.state() == OgSampleLoader::State::Done);
  const o3os::app::SampleLoadReport& r = sl.report();
  CHECK(r.loaded == 2u);
  CHECK(r.missing == 1u);
  CHECK(r.no_room == 0u);
  CHECK(r.approximated == 0u);  // the .pti's granular mode is bound since OS-GRAN
  CHECK(r.storage_error == hs::Status::Ok);
  NEED(r.problem_count == 1u);
  CHECK(r.problems[0].slot == 1u);
  CHECK(r.problems[0].why == SampleProblem::NotFound);
  // The .pti is placed first (the directory listing comes before the WAVs), then the WAV; each starts on an even sample.
  CHECK(r.samples == kPtiFrames + 65536u);

  // The .pti: slot 2, the whole sample, one shot, volume 100 is +24 dB under the 1.2+ law of a version 16 project, tune -24 and fine tune 100.
  const SampleDesc& hat = p.sample(2);
  CHECK(hat.data == arena.data());
  CHECK(hat.length == kPtiFrames);
  CHECK(hat.loop_end == 0u);
  CHECK(hat.gain == 1038676);
  CHECK(hat.tune == -24);
  CHECK(hat.fine_tune == 100);
  // Its PCM is the file's bytes after the head: the fixture's byte i is (i * 7 + 3) & 255, little-endian halfwords.
  for (std::uint32_t k = 0; k < kPtiFrames; k += 97u) {
    const std::uint32_t i = 2u * k;
    const std::int16_t want = static_cast<std::int16_t>(static_cast<std::uint16_t>(((i * 7u + 3u) & 255u) | ((((i + 1u) * 7u + 3u) & 255u) << 8)));
    CHECK(arena[k] == want);
  }

  // The WAV: slot 0, window [1000, 60000), loop [20000, 40000) of the sample, so [19000, 39000) of the window; volume 80 is +14.4 dB.
  const SampleDesc& loop = p.sample(0);
  CHECK(loop.data == arena.data() + kPtiFrames + 1000u);
  CHECK(loop.length == 59000u);
  CHECK(loop.loop_start == 19000u);
  CHECK(loop.loop_end == 39000u);
  CHECK(loop.gain == o3os::audio::level_gain_q16(80, o3os::audio::LevelLaw::Og12));
  CHECK(std::fabs(static_cast<double>(loop.gain) - std::pow(10.0, 14.4 / 20.0) * 65536.0) <= 1.0);
  CHECK(loop.tune == 3);
  CHECK(loop.fine_tune == -20);
  for (std::uint32_t k = 0; k < kWavFrames; k += 251u) {
    CHECK(arena[kPtiFrames + k] == pcm_at(k));  // the PCM as the file holds it (16-bit mono is not converted)
  }

  // The instrument without a file is silent: the table says so, and a note on it is ignored.
  CHECK(p.sample(1).length == 0u);
  CHECK(p.sample(1).data == nullptr);
  CHECK(p.has_samples());

  // Nothing was left open and the card is as it was.
  CHECK(card.open_files() == 0u);
  CHECK(card.open_dirs() == 0u);
  CHECK(snapshot(tmp.path) == before);

  char line[96];
  const std::size_t n = o3os::app::describe_samples(r, line, sizeof line);
  CHECK(std::string(line, n) == "samples 2 loaded, 1 missing, 130 KB");
  const std::size_t m = o3os::app::describe_problem(r.problems[0], line, sizeof line);
  CHECK(std::string(line, m) == "load: instrument 02 sample not found");
}

TEST_CASE("og samples: an arena that holds the .pti and not the WAV leaves that instrument silent and counts it") {
  TempDir tmp;
  write_project(tmp.path);
  DirStorage card(tmp.path.string());
  NEED(card.mount() == hs::Status::Ok);
  std::vector<std::int16_t> arena(1500, std::int16_t{-1});
  VoicePool& p = pool();
  OgSampleLoader& sl = sample_loader();
  o3os::model::ModelArena& model = o3os::test::og::arena(0);

  unsigned steps = 0;
  NEED(load_all(card, arena, p, sl, loader(), model, 4096, steps));
  NEED(sl.state() == OgSampleLoader::State::Done);
  const o3os::app::SampleLoadReport& r = sl.report();
  CHECK(r.loaded == 1u);
  CHECK(r.no_room == 1u);  // the WAV (slot 0): the importer says it does not fit before it writes a byte
  CHECK(r.missing == 1u);  // the one with no file
  CHECK(r.samples == kPtiFrames);
  CHECK(p.sample(2).length == kPtiFrames);
  CHECK(p.sample(0).length == 0u);
  for (std::size_t k = kPtiFrames; k < arena.size(); ++k) {
    CHECK(arena[k] == -1);  // nothing was written past what was placed
  }
  CHECK(card.open_files() == 0u);
  CHECK(card.open_dirs() == 0u);
}

namespace {

// A DirStorage that reports a modification stamp (DirStorage has none): a FAT volume's date << 16 | time stand-in.
class StampedStorage final : public hs::Storage {
 public:
  explicit StampedStorage(DirStorage& inner) : in_(inner) {}
  static constexpr std::uint32_t kStamp = 0x5A5A1234u;
  hs::Status mount() noexcept override { return in_.mount(); }
  bool mounted() const noexcept override { return in_.mounted(); }
  bool card_present() const noexcept override { return in_.card_present(); }
  hs::VolumeInfo volume() const noexcept override { return in_.volume(); }
  hs::Status exists(const char* path, bool* is_dir) noexcept override { return in_.exists(path, is_dir); }
  hs::Status stat(const char* path, hs::FileStamp& out) noexcept override {
    const hs::Status s = in_.stat(path, out);
    out.stamp = s == hs::Status::Ok ? kStamp : 0u;
    return s;
  }
  hs::Status open(const char* path, hs::FileHandle& out) noexcept override { return in_.open(path, out); }
  hs::Status read(hs::FileHandle f, void* dst, std::size_t max, std::size_t& got) noexcept override { return in_.read(f, dst, max, got); }
  hs::Status seek(hs::FileHandle f, std::uint32_t offset) noexcept override { return in_.seek(f, offset); }
  hs::Status size(hs::FileHandle f, std::uint32_t& out) const noexcept override { return in_.size(f, out); }
  void close(hs::FileHandle& f) noexcept override { in_.close(f); }
  hs::Status opendir(const char* path, hs::DirHandle& out) noexcept override { return in_.opendir(path, out); }
  hs::Status readdir(hs::DirHandle d, hs::DirEntry& out) noexcept override { return in_.readdir(d, out); }
  void closedir(hs::DirHandle& d) noexcept override { in_.closedir(d); }

 private:
  DirStorage& in_;
};

}  // namespace

TEST_CASE("og samples: a WAV read from the project's folder is marked on the bank with the file's size and stamp; a .pti's slot is not (OS-BUG-REMOUNT)") {
  TempDir tmp;
  write_project(tmp.path);
  DirStorage dir(tmp.path.string());
  NEED(dir.mount() == hs::Status::Ok);
  StampedStorage card(dir);
  std::vector<std::int16_t> arena(200000, std::int16_t{-1});
  VoicePool& p = pool();
  OgSampleLoader& sl = sample_loader();
  OgFolderLoader& pl = loader();
  o3os::model::ModelArena& model = o3os::test::og::arena(0);
  auto mem = std::make_unique<o3os::app::SampleBankMem>();
  o3os::app::SampleBank bank;
  bank.init(p, model, *mem, arena.data(), static_cast<std::uint32_t>(arena.size()));
  model.project.reset();
  NEED(pl.begin(card, "Workspace", model));
  NEED(run_loader(pl, 4096) == OgFolderLoader::State::Done);
  const o3os::audio::LevelLaw law = o3os::app::instrument_law(pl.report().version);
  bank.set_law(law);
  NEED(sl.begin(card, pl.folder(), model.project, pl.pti_slots(), p, arena.data(), static_cast<std::uint32_t>(arena.size()), law, &bank));
  for (unsigned n = 0; n < 100000u && sl.state() == OgSampleLoader::State::Running; ++n) {
    (void)sl.step(4096);
  }
  NEED(sl.state() == OgSampleLoader::State::Done);
  CHECK(bank.card_version(0) == bank.version(0));
  CHECK(bank.card_version(0) != 0u);
  CHECK(bank.card_size(0) == 44u + 2u * kWavFrames);
  CHECK(bank.card_stamp(0) == StampedStorage::kStamp);
  CHECK(bank.card_version(2) == 0u);  // the .pti's audio is not in a WAV
  CHECK(bank.card_size(2) == 0u);
  CHECK(bank.card_stamp(2) == 0u);
}

// ---- OS-007h: the play modes that are not "a window at the note's pitch" ----------------------------------------------------------------------------

namespace {

o3os::model::Instrument sample_instrument(std::uint8_t mode) {
  o3os::model::Instrument in{};
  in.kind = o3os::model::InstrumentKind::Sample;
  in.sampler.play_mode = mode;
  in.sampler.end = 65535;
  in.sampler.loop_end = 65535;
  in.sampler.volume = 50;
  in.sampler.wavetable_window_size = 2048;
  return in;
}

std::uint32_t bound(const std::uint16_t* table, unsigned i) { return table[2 * i] | (static_cast<std::uint32_t>(table[2 * i + 1]) << 16); }

}  // namespace

TEST_CASE("og samples: slice mode binds the selected slice, beat slice binds the whole sample and a table of boundaries, wavetable binds the current window (OS-007h)") {
  static std::int16_t pcm[100000];
  using o3os::app::make_sample_desc;
  using o3os::app::slice_table_samples;
  bool approx = true;

  // Slice (4): slices at 0, 10000, 30000, 65535 of 0..65535 of a 100,000-frame sample; the third (index 2) is selected: [30000 * 100000 / 65535, the end).
  o3os::model::Instrument sl = sample_instrument(4);
  sl.sampler.slice_count = 3;
  sl.sampler.slices[0] = 0;
  sl.sampler.slices[1] = 10000;
  sl.sampler.slices[2] = 30000;
  sl.sampler.selected_slice = 2;
  sl.sampler.start = 40000;  // not used by the slice modes
  sl.sampler.end = 50000;
  SampleDesc d = make_sample_desc(sl, pcm, 100000, &approx);
  CHECK_FALSE(approx);
  const std::uint32_t s2 = static_cast<std::uint32_t>(30000.0f * (100000.0f / 65535.0f));
  CHECK(d.data == pcm + s2);
  CHECK(d.length == 100000u - s2);
  CHECK(d.offset == s2);
  CHECK_FALSE(d.beat_slice);
  CHECK(d.loop_end == 0u);
  sl.sampler.selected_slice = 1;  // [10000, 30000)
  d = make_sample_desc(sl, pcm, 100000, &approx);
  CHECK(d.data == pcm + static_cast<std::uint32_t>(10000.0f * (100000.0f / 65535.0f)));
  CHECK(d.length == s2 - static_cast<std::uint32_t>(10000.0f * (100000.0f / 65535.0f)));
  sl.sampler.slice_count = 0;  // no slices: the whole sample
  d = make_sample_desc(sl, pcm, 100000, &approx);
  CHECK(d.data == pcm);
  CHECK(d.length == 100000u);
  sl.sampler.slice_count = 3;
  sl.sampler.selected_slice = 1;
  sl.sampler.slices[2] = 5000;  // the next slice starts before this one: the slice runs to the end of the sample (this OS's rule)
  d = make_sample_desc(sl, pcm, 100000, &approx);
  CHECK(d.data == pcm + static_cast<std::uint32_t>(10000.0f * (100000.0f / 65535.0f)));
  CHECK(d.data + d.length == pcm + 100000);

  // Beat slice (5): the table has slice_count + 1 boundaries, as two halves each, the last one the length; a slice that fits no table is the whole sample.
  o3os::model::Instrument bs = sample_instrument(5);
  bs.sampler.slice_count = 3;
  bs.sampler.slices[0] = 1000;
  bs.sampler.slices[1] = 20000;
  bs.sampler.slices[2] = 40000;
  CHECK(slice_table_samples(bs) == 8u);
  CHECK(slice_table_samples(sample_instrument(5)) == 0u);  // no slices: no table
  CHECK(slice_table_samples(sl) == 0u);                    // only a beat slice has one
  std::int16_t table[8];
  d = make_sample_desc(bs, pcm, 100000, &approx, table);
  CHECK_FALSE(approx);
  CHECK(d.beat_slice);
  CHECK(d.data == pcm);
  CHECK(d.length == 100000u);
  CHECK(d.slice_count == 3u);
  NEED(d.slices != nullptr);
  CHECK(bound(d.slices, 0) == static_cast<std::uint32_t>(1000.0f * (100000.0f / 65535.0f)));
  CHECK(bound(d.slices, 1) == static_cast<std::uint32_t>(20000.0f * (100000.0f / 65535.0f)));
  CHECK(bound(d.slices, 2) == static_cast<std::uint32_t>(40000.0f * (100000.0f / 65535.0f)));
  CHECK(bound(d.slices, 3) == 100000u);
  d = make_sample_desc(bs, pcm, 100000, &approx, nullptr);  // no room for the table was given: the whole sample at the root's pitch, and it says so
  CHECK(approx);
  CHECK(d.beat_slice);
  CHECK(d.slice_count == 0u);
  CHECK(d.length == 100000u);
  // a sample longer than 65,535 frames: a boundary needs both halves
  static std::int16_t big_table[8];
  bs.sampler.slices[2] = 65535;
  const std::uint32_t frames = 3000000u;
  d = make_sample_desc(bs, pcm, frames, &approx, big_table);
  CHECK(bound(d.slices, 2) == frames);
  CHECK(bound(d.slices, 3) == frames);

  // Wavetable (6): the current window as a loop, with the wavetable's pitch law; the window is clamped to the last whole one; start and end are not used.
  o3os::model::Instrument wt = sample_instrument(6);
  wt.sampler.wavetable_window_size = 2048;
  wt.sampler.wavetable_window = 3;
  d = make_sample_desc(wt, pcm, 20 * 2048u + 100u, &approx);
  CHECK_FALSE(approx);
  CHECK(d.data == pcm + 3 * 2048u);
  CHECK(d.length == 2048u);
  CHECK(d.loop_start == 0u);
  CHECK(d.loop_end == 2048u);
  CHECK(d.wt_size == 2048u);
  CHECK(d.offset == 3 * 2048u);
  CHECK(d.wt_windows == 20u);  // OS-GRAN: the stack's windows and the one bound
  CHECK(d.wt_pos == 3u);
  wt.sampler.wavetable_window = 500;  // past the table: the last whole window (19), the 100 frames after it are not a window
  d = make_sample_desc(wt, pcm, 20 * 2048u + 100u, &approx);
  CHECK(d.data == pcm + 19 * 2048u);
  CHECK(d.wt_pos == 19u);
  wt.sampler.wavetable_window_size = 300;  // not a window size: the 2048 table is kept
  d = make_sample_desc(wt, pcm, 20 * 2048u, &approx);
  CHECK(d.wt_size == 2048u);
  wt.sampler.wavetable_window_size = 256;
  d = make_sample_desc(wt, pcm, 100u, &approx);  // less than one window: nothing plays
  CHECK(d.length == 0u);
  d = make_sample_desc(wt, pcm, 256u, &approx);  // exactly one
  CHECK(d.length == 256u);
  CHECK(d.data == pcm);

  // Granular (7; OS-GRAN): the WHOLE sample (start, end and the loop are not used), the grain's length, position and shape, its type as the loop mode; nothing is approximated.
  o3os::model::Instrument gr = sample_instrument(7);
  gr.sampler.start = 20000;  // not used by the engine's granular mode
  gr.sampler.end = 30000;
  gr.sampler.grain_length = 1234;
  gr.sampler.grain_position = 40000;
  gr.sampler.grain_shape = 2;
  gr.sampler.grain_type = 1;
  approx = true;
  d = make_sample_desc(gr, pcm, 1000, &approx);
  CHECK_FALSE(approx);
  CHECK(d.granular);
  CHECK(d.length == 1000u);
  CHECK(d.data == pcm);
  CHECK(d.gr_len == 1234u);
  CHECK(d.gr_pos == 40000u);
  CHECK(d.gr_shape == 2u);
  CHECK(d.loop_mode == 1u);
  CHECK(d.loop_end == 0u);
  CHECK_FALSE(d.beat_slice);
}

TEST_CASE("og samples: backward and ping-pong bind the loop with the direction, the pan and the amp envelope are converted to the OG's units (OS-007k)") {
  static std::int16_t pcm[1000];
  using o3os::app::make_sample_desc;
  bool approx = true;

  // modes 1, 2 and 3 share the loop conditions: [loop1, loop2) inside [start, end); a loop that fails them is a one shot, whatever the mode
  for (const unsigned mode : {1u, 2u, 3u}) {
    o3os::model::Instrument in = sample_instrument(static_cast<std::uint8_t>(mode));
    in.sampler.loop_start = 16384;  // 1/4 of 1000 frames
    in.sampler.loop_end = 49151;    // 3/4
    approx = true;
    SampleDesc d = make_sample_desc(in, pcm, 1000, &approx);
    CHECK_FALSE(approx);
    CHECK(d.loop_start == 250u);
    CHECK(d.loop_end == 749u);
    CHECK(d.loop_mode == mode - 1u);  // 0 forward, 1 backward, 2 ping-pong
    in.sampler.loop_start = 50000;     // after the loop end
    d = make_sample_desc(in, pcm, 1000, &approx);
    CHECK(d.loop_end == 0u);
    CHECK(d.loop_mode == 0u);
  }

  // pan
  o3os::model::Instrument in = sample_instrument(0);
  CHECK(make_sample_desc(in, pcm, 1000, nullptr).pan == 0u);  // the fixture instrument is zero-filled: panning 0 is hard left
  in.sampler.panning = 50;
  CHECK(make_sample_desc(in, pcm, 1000, nullptr).pan == 50u);
  in.sampler.panning = 250;  // out of range: kept in it
  CHECK(make_sample_desc(in, pcm, 1000, nullptr).pan == 100u);

  // the amp envelope: off, or an LFO (loop flag set): the plain gain
  in.sampler.panning = 50;
  o3os::model::Envelope& e = in.sampler.envelope[0];
  e = o3os::model::Envelope{};
  e.amount = 0.5f;
  e.delay = 0;
  e.attack = 17;  // 17 ms = 749 samples at 44,100 Hz (750 at 44117.647 Hz): ((749 + 7) >> 3) = 94 groups at either
  e.hold = 0;
  e.decay = 300;  // 13230 samples: 1654 groups at 44,100 Hz (13235 samples: 1655 at 44117.647 Hz)
  e.sustain = 0.25f;
  e.release = 65535;  // 2891 s of samples: the time is held at 65535 groups
  e.enable = 0;
  CHECK_FALSE(make_sample_desc(in, pcm, 1000, nullptr).env.enable);
  e.enable = 1;
  e.loop = 1;  // this slot is the LFO of lfo[0]: TODO(OS-007k), the plain gain
  CHECK_FALSE(make_sample_desc(in, pcm, 1000, nullptr).env.enable);
  e.loop = 0;
  const SampleDesc d = make_sample_desc(in, pcm, 1000, nullptr);
  CHECK(d.env.enable);
  CHECK(d.env.delay == 1u);  // 0 ms is one group (the time is at least 1)
  CHECK(d.env.attack == 94u);
  CHECK(d.env.hold == 1u);
  CHECK(d.env.decay == ref_groups(300));
  CHECK(d.env.release == 65535u);
  CHECK(d.env.sustain == 268435456u);  // 0.25 in Q30
  CHECK(d.env.amount == 32768);        // 0.5 in Q16
  // the unit against the reference formula in double, for every millisecond count a u16 holds
  for (unsigned ms = 0; ms <= 65535u; ms += 1u) {
    e.attack = static_cast<std::uint16_t>(ms);
    const std::uint64_t units = ref_groups(ms);
    if (make_sample_desc(in, pcm, 1000, nullptr).env.attack != static_cast<std::uint16_t>(units)) {
      FAIL("attack " << ms << " ms is not " << units << " groups");
    }
  }

  // the filter (OS-007l): the .pti's enable, type, cutoff and resonance as stored; a disabled one is carried too (the voices bypass it)
  CHECK_FALSE(make_sample_desc(in, pcm, 1000, nullptr).filter.enable);
  in.sampler.filter_enable = 1;
  in.sampler.filter_type = 2;
  in.sampler.cutoff = 0.62f;
  in.sampler.resonance = 3.1f;
  const SampleDesc f = make_sample_desc(in, pcm, 1000, nullptr);
  CHECK(f.filter.enable);
  CHECK(f.filter.type == 2u);
  CHECK(f.filter.cutoff == 0.62f);
  CHECK(f.filter.resonance == 3.1f);

  // the cutoff modulator (OS-007m): slot 2 is the ADSR when it is enabled and not looping, the LFO of lfo[2] when its loop byte is set, nothing when it is off
  namespace audio = o3os::audio;
  CHECK(f.filter.mod.kind == audio::kFmodNone);
  in.sampler.envelope[2] = o3os::model::Envelope{0.7f, 11, 22, 33, 44, 0.25f, 55, 0, 0};  // not enabled: no modulator, whatever else it holds
  CHECK(make_sample_desc(in, pcm, 1000, nullptr).filter.mod.kind == audio::kFmodNone);
  in.sampler.envelope[2].enable = 1;
  const audio::FilterMod a = make_sample_desc(in, pcm, 1000, nullptr).filter.mod;
  CHECK(a.kind == audio::kFmodEnv);
  CHECK(a.amount == 0.7f);
  CHECK(a.sustain == 0.25f);
  CHECK(a.delay == 11u);
  CHECK(a.attack == 22u);
  CHECK(a.hold == 33u);
  CHECK(a.decay == 44u);
  CHECK(a.release == 55u);
  in.sampler.envelope[2].loop = 1;
  in.sampler.lfo[2] = o3os::model::Lfo{3, 17, 0.35f};
  const audio::FilterMod l = make_sample_desc(in, pcm, 1000, nullptr).filter.mod;
  CHECK(l.kind == audio::kFmodLfo);
  CHECK(l.shape == 3u);
  CHECK(l.rate == 17u);
  CHECK(l.amount == 0.35f);  // the LFO's own amount, not the envelope's 0.7
  CHECK(l.lfo_amount == 0.35f);
  // OS-FU2: the Filter LFO FX plays lfo[2]'s shape and amount whatever the instrument's own modulator is: an ADSR slot, and a slot that is off, carry them too
  in.sampler.envelope[2].loop = 0;
  const audio::FilterMod ea = make_sample_desc(in, pcm, 1000, nullptr).filter.mod;
  CHECK(ea.kind == audio::kFmodEnv);
  CHECK(ea.amount == 0.7f);
  CHECK(ea.shape == 3u);
  CHECK(ea.lfo_amount == 0.35f);
  in.sampler.envelope[2].enable = 0;
  const audio::FilterMod n = make_sample_desc(in, pcm, 1000, nullptr).filter.mod;
  CHECK(n.kind == audio::kFmodNone);
  CHECK(n.shape == 3u);
  CHECK(n.lfo_amount == 0.35f);
}

TEST_CASE("og samples: a beat slice instrument's table follows its PCM in the arena; one that does not fit is silent, and `diag inst` says what is bound (OS-007h)") {
  TempDir tmp;
  write_file(tmp.path, "Workspace/project.mt", project_bytes(ProjectSpec{}));
  InstrumentSpec pti;
  pti.pti = true;
  pti.frames = 1001;  // odd: the next sample starts on an even index
  pti.file_name = "Break";
  Bytes f = instrument_file(pti, 5, 0, 0, 65535, 65535, 0, 0, 80);
  f[376] = 3;  // three slices at 0, 20000 and 40000 of 65535
  p16(f, 280, 0);
  p16(f, 282, 20000);
  p16(f, 284, 40000);
  p32(f, 388, o3os::formats::crc32(f.data(), 388));
  write_file(tmp.path, "Workspace/instruments/instrument_01.pti", f);
  write_file(tmp.path, "Workspace/instruments/instrument_02.pti", instrument_file(pti, 0, 0, 0, 65535, 65535, 0, 0, 100));
  DirStorage card(tmp.path.string());
  NEED(card.mount() == hs::Status::Ok);

  // room for everything: slot 0's PCM (1002 samples with the pad), its table (8), slot 1's PCM (1002)
  {
    std::vector<std::int16_t> arena(1002 + 8 + 1002, std::int16_t{-1});
    VoicePool& p = pool();
    OgSampleLoader& sl = sample_loader();
    unsigned steps = 0;
    NEED(load_all(card, arena, p, sl, loader(), o3os::test::og::arena(0), 4096, steps));
    CHECK(sl.report().loaded == 2u);
    CHECK(sl.report().no_room == 0u);
    CHECK(sl.report().approximated == 0u);
    CHECK(sl.report().samples == 1002u + 8u + 1002u);
    const SampleDesc& d = p.sample(0);
    CHECK(d.beat_slice);
    CHECK(d.slice_count == 3u);
    NEED(d.slices != nullptr);
    // The .pti files are placed in the order of the directory listing (the host's is the file system's: either may be first); the table is right after its PCM.
    const std::ptrdiff_t at = d.data - arena.data();
    CHECK((at == 0 || at == 1002));
    CHECK(reinterpret_cast<const std::int16_t*>(d.slices) == arena.data() + at + 1002);  // on an even index
    CHECK(bound(d.slices, 0) == 0u);
    CHECK(bound(d.slices, 1) == static_cast<std::uint32_t>(20000.0f * (1001.0f / 65535.0f)));
    CHECK(bound(d.slices, 3) == 1001u);
    CHECK_FALSE(p.sample(1).beat_slice);
    CHECK(p.sample(1).data == (at == 0 ? arena.data() + 1002 + 8 : arena.data()));  // the next sample starts after the table
    // `diag inst 1`: the model's instrument 0 and the voices' descriptor
    char line[160];
    const std::size_t n = o3os::app::describe_instrument(0, o3os::test::og::arena(0).project.instrument(0), d, line, sizeof line);
    std::uint32_t sum = 0;
    for (unsigned i = 0; i < 64; ++i) {
      sum += static_cast<std::uint16_t>(arena[static_cast<std::size_t>(at) + i]);
    }
    char want[160];
    std::snprintf(want, sizeof want, "inst 01: mode 5 start 0 len 1001 loop 0..0 root 60 rate 44100 vol 80 gain +14.4 dB beat slices 3 first-note 24 sum %04x", sum & 0xFFFFu);
    CHECK(std::string(line, n) == want);
  }
  // room for one PCM and not for the table: slot 0 is silent (no room, whichever of the two is listed first), slot 1 still loads
  {
    std::vector<std::int16_t> arena(1002 + 7, std::int16_t{-1});
    VoicePool& p = pool();
    OgSampleLoader& sl = sample_loader();
    unsigned steps = 0;
    NEED(load_all(card, arena, p, sl, loader(), o3os::test::og::arena(0), 4096, steps));
    CHECK(sl.report().loaded == 1u);
    CHECK(sl.report().no_room == 1u);
    CHECK(p.sample(0).length == 0u);
    CHECK(p.sample(1).length == 1001u);
    CHECK(p.sample(1).data == arena.data());  // the one that did not fit left the arena as it was
  }
}

TEST_CASE("og samples: `diag inst` lines for a plain sample, a wavetable, an empty slot and a slot that is not a sample (OS-007h)") {
  static std::int16_t pcm[4096];
  for (unsigned i = 0; i < 4096; ++i) {
    pcm[i] = static_cast<std::int16_t>(i - 2000);
  }
  char line[160];
  const auto text = [&](unsigned slot, const o3os::model::Instrument* in, const SampleDesc& d) {
    const std::size_t n = o3os::app::describe_instrument(slot, in, d, line, sizeof line);
    CHECK(n == std::strlen(line));
    return std::string(line, n);
  };
  o3os::model::Instrument in = sample_instrument(0);
  in.sampler.start = 0;
  in.sampler.volume = 100;
  const SampleDesc plain = o3os::app::make_sample_desc(in, pcm, 4096, nullptr);
  std::uint32_t sum = 0;
  for (unsigned i = 0; i < 64; ++i) {
    sum += static_cast<std::uint16_t>(pcm[i]);
  }
  char want[160];
  std::snprintf(want, sizeof want, "inst 03: mode 0 start 0 len 4096 loop 0..0 root 60 rate 44100 vol 100 gain +24.0 dB sum %04x", sum & 0xFFFFu);
  CHECK(text(2, &in, plain) == want);
  in.sample_rate = 22050;
  std::snprintf(want, sizeof want, "inst 12: mode 0 start 0 len 4096 loop 0..0 root 60 rate 22050 vol 100 gain +24.0 dB sum %04x", sum & 0xFFFFu);
  CHECK(text(11, &in, plain) == want);
  o3os::model::Instrument wt = sample_instrument(6);
  wt.sampler.wavetable_window_size = 1024;
  wt.sampler.wavetable_window = 2;
  const SampleDesc wd = o3os::app::make_sample_desc(wt, pcm, 4096, nullptr);
  sum = 0;
  for (unsigned i = 0; i < 64; ++i) {
    sum += static_cast<std::uint16_t>(pcm[2048 + i]);
  }
  std::snprintf(want, sizeof want, "inst 04: mode 6 start 2048 len 1024 loop 0..1024 root 60 rate 44100 vol 50 gain +0.0 dB wt 1024 win 2/4 sum %04x", sum & 0xFFFFu);
  CHECK(text(3, &wt, wd) == want);
  CHECK(text(4, &in, SampleDesc{}) == "inst 05: mode 0, silent: nothing is bound vol 100 gain +0.0 dB");  // no law passed: the descriptor's own gain (unity) with the stored level (OS-BUG-PASTE24 E2)
  CHECK(text(5, nullptr, SampleDesc{}) == "inst 06: not a sample instrument");
  o3os::model::Instrument midi{};
  midi.kind = o3os::model::InstrumentKind::Midi;
  CHECK(text(63, &midi, SampleDesc{}) == "inst 64: not a sample instrument");
  // a cap too small to hold the line: cut, terminated
  char tiny[12];
  const std::size_t n = o3os::app::describe_instrument(2, &in, plain, tiny, sizeof tiny);
  CHECK(n == 11u);
  CHECK(tiny[11] == '\0');
}

TEST_CASE("og samples: the instrument volume law follows the project file's version; `diag inst` says the level and the gain in dB (OS-007i)") {
  using o3os::app::instrument_law;
  using o3os::audio::level_gain_q16;
  using o3os::audio::LevelLaw;
  // version 7 is the first to use our Og12 law ("the 1.2+ law"); 6 and below use our Og1114 law ("the 1.1.14 table"); both laws are this OS's model (and 0 is no project read)
  for (const unsigned v : {0u, 2u, 3u, 4u, 5u, 6u}) {
    CHECK(instrument_law(static_cast<std::uint8_t>(v)) == LevelLaw::Og1114);
  }
  for (const unsigned v : {7u, 11u, 14u, 16u, 17u, 18u, 255u}) {
    CHECK(instrument_law(static_cast<std::uint8_t>(v)) == LevelLaw::Og12);
  }
  // the default instrument (volume 50): 0 dB in a 1.2+ project, -20 dB in an older one; volume 100 the other way round
  static std::int16_t pcm[4096] = {};
  o3os::model::Instrument in = sample_instrument(0);
  in.sampler.start = 0;
  CHECK(o3os::app::make_sample_desc(in, pcm, 4096, nullptr, nullptr, LevelLaw::Og12).gain == 65536);
  CHECK(o3os::app::make_sample_desc(in, pcm, 4096, nullptr, nullptr, LevelLaw::Og1114).gain == 6554);
  in.sampler.volume = 100;
  CHECK(o3os::app::make_sample_desc(in, pcm, 4096, nullptr, nullptr, LevelLaw::Og1114).gain == 65536);
  CHECK(o3os::app::make_sample_desc(in, pcm, 4096, nullptr, nullptr, LevelLaw::Og12).gain == 1038676);
  in.sampler.volume = 0;
  CHECK(o3os::app::make_sample_desc(in, pcm, 4096, nullptr, nullptr, LevelLaw::Og12).gain == 0);
  // the dB of `diag inst` (a tenth, half away from zero), the silent level and the lowest
  char line[160];
  const auto db_of = [&](std::uint8_t volume, LevelLaw law) {
    in.sampler.volume = volume;
    const SampleDesc d = o3os::app::make_sample_desc(in, pcm, 4096, nullptr, nullptr, law);
    const std::size_t n = o3os::app::describe_instrument(0, &in, d, line, sizeof line);
    const std::string text(line, n);
    const std::size_t at = text.find(" vol ");
    return at == std::string::npos ? std::string() : text.substr(at + 1, text.find(" sum ") - at - 1);
  };
  CHECK(db_of(0, LevelLaw::Og12) == "vol 0 gain -inf dB");
  CHECK(db_of(1, LevelLaw::Og12) == "vol 1 gain -24.0 dB");
  CHECK(db_of(2, LevelLaw::Og12) == "vol 2 gain -23.0 dB");
  CHECK(db_of(50, LevelLaw::Og12) == "vol 50 gain +0.0 dB");
  CHECK(db_of(51, LevelLaw::Og12) == "vol 51 gain +0.5 dB");
  CHECK(db_of(100, LevelLaw::Og12) == "vol 100 gain +24.0 dB");
  CHECK(db_of(50, LevelLaw::Og1114) == "vol 50 gain -20.0 dB");
  CHECK(db_of(100, LevelLaw::Og1114) == "vol 100 gain +0.0 dB");

  // the loader hands the law to every instrument it binds: the same card, the 1.1.14 table
  TempDir tmp;
  write_project(tmp.path);
  DirStorage card(tmp.path.string());
  NEED(card.mount() == hs::Status::Ok);
  std::vector<std::int16_t> arena(100000, std::int16_t{-1});
  VoicePool& p = pool();
  OgSampleLoader& sl = sample_loader();
  OgFolderLoader& pl = loader();
  o3os::model::ModelArena& model = o3os::test::og::arena(0);
  model.project.reset();
  NEED(pl.begin(card, "Workspace", model));
  NEED(run_loader(pl, 4096) == OgFolderLoader::State::Done);
  NEED(sl.begin(card, pl.folder(), model.project, pl.pti_slots(), p, arena.data(), static_cast<std::uint32_t>(arena.size()), LevelLaw::Og1114));
  for (unsigned n = 0; n < 100000u && sl.state() == OgSampleLoader::State::Running; ++n) {
    (void)sl.step(4096);
  }
  CHECK(p.sample(2).gain == 65536);  // volume 100
  CHECK(p.sample(0).gain == level_gain_q16(80, LevelLaw::Og1114));
  CHECK(std::fabs(static_cast<double>(p.sample(0).gain) - std::pow(10.0, -20.0 / 50.0) * 65536.0) <= 1.0);
}

TEST_CASE("og samples: the wavetable (slot 3) and granular (slot 4) position envelopes are bound as the position modulator of their kind of instrument only (OS-GRAN)") {
  static std::int16_t pcm[4096];
  using o3os::app::make_sample_desc;
  namespace audio = o3os::audio;
  o3os::model::Instrument wt = sample_instrument(6);
  wt.sampler.wavetable_window_size = 256;
  wt.sampler.envelope[3].enable = 1;
  wt.sampler.envelope[3].amount = 0.4f;
  wt.sampler.envelope[3].attack = 500;
  wt.sampler.envelope[3].sustain = 0.7f;
  wt.sampler.envelope[4].enable = 1;  // the granular slot of a wavetable: not run
  SampleDesc d = make_sample_desc(wt, pcm, 4096, nullptr);
  CHECK(d.pos_mod.kind == audio::kFmodEnv);
  CHECK(d.pos_mod.amount == doctest::Approx(0.4f));
  CHECK(d.pos_mod.attack == 500u);
  CHECK(d.pos_mod.sustain == doctest::Approx(0.7f));
  CHECK(d.wt_windows == 16u);
  o3os::model::Instrument gr = sample_instrument(7);
  gr.sampler.envelope[3].enable = 1;  // the wavetable slot of a granular instrument: not run
  d = make_sample_desc(gr, pcm, 4096, nullptr);
  CHECK(d.pos_mod.kind == audio::kFmodNone);
  gr.sampler.envelope[4].enable = 1;
  gr.sampler.envelope[4].loop = 1;  // an LFO: shape, rate and amount from lfo[4]
  gr.sampler.lfo[4].shape = 3;
  gr.sampler.lfo[4].speed = 9;
  gr.sampler.lfo[4].amount = 0.25f;
  d = make_sample_desc(gr, pcm, 4096, nullptr);
  CHECK(d.pos_mod.kind == audio::kFmodLfo);
  CHECK(d.pos_mod.shape == 3u);
  CHECK(d.pos_mod.rate == 9u);
  CHECK(d.pos_mod.amount == doctest::Approx(0.25f));
  o3os::model::Instrument plain = sample_instrument(0);
  plain.sampler.envelope[3].enable = 1;
  plain.sampler.envelope[4].enable = 1;
  d = make_sample_desc(plain, pcm, 4096, nullptr);
  CHECK(d.pos_mod.kind == audio::kFmodNone);
}

// OS-BUG-PASTE24: a descriptor rebuilt from the project with nothing changed must equal the one the load made, for the law of every project version.
namespace {

// The fields a rebuild must reproduce. (Not a memcmp of the struct: SampleDesc has padding that nothing zeroes, so two equal descriptors can differ in those bytes. A first version of this case compared
// the bytes, passed alone and failed in a run after other cases for that reason; zeroing the padding costs a descriptor of flash for a property only a test reads, so the fields are compared.)
bool same_desc(const SampleDesc& a, const SampleDesc& b) {
  return a.data == b.data && a.length == b.length && a.offset == b.offset && a.loop_start == b.loop_start && a.loop_end == b.loop_end && a.gain == b.gain && a.tune == b.tune &&
         a.fine_tune == b.fine_tune && a.pan == b.pan && a.engine == b.engine && a.amiga_vol == b.amiga_vol && a.rate_q28 == b.rate_q28 && a.env.enable == b.env.enable &&
         a.env.amount == b.env.amount && a.filter.enable == b.filter.enable && a.filter.type == b.filter.type && a.filter.cutoff == b.filter.cutoff &&
         a.filter.resonance == b.filter.resonance;
}

void write_vol100_project(const fs::path& root, unsigned version) {
  ProjectSpec ps;
  ps.version = version;
  write_file(root, "Workspace/project.mt", project_bytes(ps));
  InstrumentSpec mti;
  mti.pti = false;
  mti.file_name = "Loud";
  write_file(root, "Workspace/instruments/instrument_01.mti", instrument_file(mti, 0, 0, 0, 65535, 65535, 0, 0, 100));
  write_file(root, "Workspace/samples/instr01.wav", wav_bytes(kWavFrames));
  InstrumentSpec pti;
  pti.pti = true;
  pti.frames = kPtiFrames;
  pti.file_name = "Kick";
  write_file(root, "Workspace/instruments/instrument_02.pti", instrument_file(pti, 0, 0, 0, 65535, 65535, 0, 0, 100));
}

}  // namespace

TEST_CASE("og samples: a rebuild of the descriptors with nothing changed is the load's, for every project version (OS-BUG-PASTE24)") {
  for (const unsigned version : {6u, 7u, 16u}) {
    CAPTURE(version);
    TempDir tmp;
    write_vol100_project(tmp.path, version);
    DirStorage card(tmp.path.string());
    NEED(card.mount() == hs::Status::Ok);
    std::vector<std::int16_t> arena(100000, std::int16_t{-1});
    VoicePool& p = pool();
    OgSampleLoader& sl = sample_loader();
    OgFolderLoader& pl = loader();
    o3os::model::ModelArena& model = o3os::test::og::arena(0);
    auto mem = std::make_unique<o3os::app::SampleBankMem>();
    o3os::app::SampleBank bank;
    bank.init(p, model, *mem, arena.data(), static_cast<std::uint32_t>(arena.size()));
    model.project.reset();
    NEED(pl.begin(card, "Workspace", model));
    NEED(run_loader(pl, 4096) == OgFolderLoader::State::Done);
    // as the platform does (app_main.cpp begin_samples)
    CHECK(pl.report().version == version);
    const o3os::audio::LevelLaw law = o3os::app::instrument_law(pl.report().version);
    CHECK(law == (version >= 7u ? o3os::audio::LevelLaw::Og12 : o3os::audio::LevelLaw::Og1114));
    NEED(o3os::app::commit_og_law_if_done(model.project, pl));  // the platform commits the document's law at the Done of the load; a rebuild reads it
    bank.set_law(law);
    NEED(sl.begin(card, pl.folder(), model.project, pl.pti_slots(), p, arena.data(), static_cast<std::uint32_t>(arena.size()), law, &bank));
    for (unsigned n = 0; n < 100000u && sl.state() == OgSampleLoader::State::Running; ++n) {
      (void)sl.step(4096);
    }
    NEED(sl.state() == OgSampleLoader::State::Done);
    const SampleDesc loaded = p.sample(0);
    CHECK(loaded.gain == o3os::audio::level_gain_q16(100, law));
    const SampleDesc loaded1 = p.sample(1);
    CHECK(loaded1.gain == o3os::audio::level_gain_q16(100, law));
    bank.request_rebind(0);
    bank.request_rebind(1);
    std::uint64_t now = 1000;
    bool wrote = false;
    for (unsigned n = 0; n < 4 && !wrote; ++n) {
      now += 10;
      wrote = bank.poll(now, true);
    }
    NEED(wrote);
    const SampleDesc rebuilt = p.sample(0);
    CHECK(same_desc(loaded, rebuilt));
    CHECK(rebuilt.gain == loaded.gain);
    const SampleDesc rebuilt1 = p.sample(1);
    CHECK(rebuilt1.gain == loaded1.gain);
    CHECK(same_desc(loaded1, rebuilt1));
  }
}

// ---- OS-BUG-PASTE24, phase 1: the acceptance tests of the fix (docs/specs/OS-BUG-PASTE24.md) ------------------------------------------------------------------
// The invariant: what a project plays does not change when it is saved native and reloaded. The law is not stored anywhere: the platform derives it at every load (app_main.cpp
// begin_samples: instrument_law(version) for an OG project, Og12 for a native one and for an import), so an OG project of a version below 7 (the 1.1.14 law, 100 = 0 dB) reloads at
// the 1.2+ law, where 100 is +24 dB. A native project written from a v7+ OG project is not affected.
namespace {

// The gain the platform gives instrument 1 of the OG project in `root` at version `version`, and of the same project saved native and loaded again (the law of the native project is the one its LAW chunk
// says: S1 + S2 of OS-BUG-PASTE24; before them a native project was always read at the 1.2+ law). `keep_instruments`: a KEEP INSTRUMENTS reset (the instruments and the law stay) comes before the save.
struct SaveRoundTrip {
  bool ok = false;
  std::int32_t og = 0;
  std::int32_t native = 0;
  bool law_chunk = false;     // the saved file carries a LAW chunk
  std::uint8_t law_back = 0;  // the law of the document the native file loaded into
};

SaveRoundTrip og_then_native(unsigned version, bool keep_instruments = false) {
  SaveRoundTrip r;
  TempDir tmp;
  write_vol100_project(tmp.path, version);
  DirStorage card(tmp.path.string());
  if (card.mount() != hs::Status::Ok) {
    return r;
  }
  OgFolderLoader& pl = loader();
  o3os::model::ModelArena& model = o3os::test::og::arena(0);
  model.project.reset();
  if (!pl.begin(card, "Workspace", model) || run_loader(pl, 4096) != OgFolderLoader::State::Done || pl.report().version != version) {
    return r;
  }
  static std::int16_t pcm[4096] = {};
  const o3os::model::Instrument* in = model.project.instrument(0);
  if (in == nullptr || in->sampler.volume != 100) {
    return r;
  }
  r.og = o3os::app::make_sample_desc(*in, pcm, 4096, nullptr, nullptr, o3os::app::instrument_law(pl.report().version)).gain;
  if (!o3os::app::commit_og_law_if_done(model.project, pl)) {  // as the platform does at the Done of an OG load (app_main.cpp begin_samples)
    return r;
  }
  if (keep_instruments) {
    model.project.reset(true);
  }
  std::vector<std::uint8_t> bytes;
  if (o3os::test::save(model.project, bytes) != o3os::formats::native::Progress::Done) {
    return r;
  }
  for (const o3os::test::RawChunk& c : o3os::test::split_chunks(bytes)) {
    r.law_chunk = r.law_chunk || c.id == o3os::formats::native::kChunkLaw;
  }
  o3os::model::ModelArena& back = o3os::test::arena(1);
  if (o3os::test::load(back, bytes) != o3os::formats::native::Progress::Done || back.project.instrument(0) == nullptr) {
    return r;
  }
  r.law_back = back.project.volume_law();
  r.native = o3os::app::make_sample_desc(*back.project.instrument(0), pcm, 4096, nullptr, nullptr, o3os::app::project_law(back.project)).gain;  // what begin_samples does for a native project
  r.ok = true;
  return r;
}

}  // namespace

TEST_CASE("og samples: an OG v7+ project at VOLUME 100 saved native and reloaded plays at the same gain (OS-BUG-PASTE24, guard)") {
  for (const unsigned version : {7u, 16u}) {
    CAPTURE(version);
    const SaveRoundTrip r = og_then_native(version);
    NEED(r.ok);
    CHECK(r.og == r.native);
    CHECK_FALSE(r.law_chunk);  // a v7+ project is the 1.2+ law: no LAW chunk, the file is the one it always was
    CHECK(r.law_back == 0);
  }
}

// An OG project of a version below 7 (1.1.14 table, 100 = 0 dB: gain 65536) saved native carries its law (the LAW chunk, S1 of OS-BUG-PASTE24) and the native load acts on it (S2): it reloads at the same gain.
// (Before them the case was marked should_fail: the copy reloaded at the 1.2+ law, +24 dB, 1038676.)
TEST_CASE("og samples: an OG v6 project at VOLUME 100 saved native and reloaded plays at the same gain (OS-BUG-PASTE24)") {
  const SaveRoundTrip r = og_then_native(6u);
  NEED(r.ok);
  CHECK(r.og == 65536);  // 0 dB as loaded
  CHECK(r.og == r.native);
  CHECK(r.law_chunk);
  CHECK(r.law_back == 1);
}

// The same through a KEEP INSTRUMENTS reset: the kept instruments and the law stay, so the SAVE that follows writes the chunk and the copy plays at the same gain.
TEST_CASE("og samples: an OG v6 project after a KEEP INSTRUMENTS reset and SAVE keeps its law in the file (OS-BUG-PASTE24)") {
  const SaveRoundTrip r = og_then_native(6u, true);
  NEED(r.ok);
  CHECK(r.og == 65536);
  CHECK(r.og == r.native);
  CHECK(r.law_chunk);
  CHECK(r.law_back == 1);
}

// ---- OS-BUG-PASTE24 phase 2: the demo kit, the document's law, D5 ---------------------------------------------------------------------------------------------
namespace {

// A bank over a fresh voices table and a fresh arena, working on the model arena `model` (the document).
struct LawRig {
  std::unique_ptr<VoicePool> voices = std::make_unique<VoicePool>();
  std::unique_ptr<o3os::app::SampleBankMem> mem = std::make_unique<o3os::app::SampleBankMem>();
  std::vector<std::int16_t> arena = std::vector<std::int16_t>(100000, std::int16_t{0});
  o3os::app::SampleBank bank;
  o3os::model::ModelArena& model;
  explicit LawRig(o3os::model::ModelArena& m) : model(m) { bank.init(*voices, model, *mem, arena.data(), static_cast<std::uint32_t>(arena.size())); }
  // every slot marked, the bank polled until it has rebuilt them (the engine is quiet)
  bool rebuild_all() {
    for (unsigned i = 0; i < 64; ++i) {
      bank.request_rebind(i);
    }
    std::uint64_t now = 1000;
    for (unsigned n = 0; n < 6; ++n) {
      now += 10;
      if (bank.poll(now, true)) {
        return true;
      }
    }
    return false;
  }
};

}  // namespace

// The simpler root cause (the risk review of OS-BUG-PASTE24): a project with no sample instrument (an OG project of MIDI instruments such as t-v16, or the boot) gets the demo kit, which is bound at
// unity (+0 dB) while its four instruments said VOLUME 100 (KICK among them): the first rebuild of a descriptor (any Params edit) read 100 through the 1.2+ law, +24 dB. The instruments are now at the
// level that is 0 dB under the document's law, and a rebuild is the kit's descriptor.
TEST_CASE("og samples: the demo kit's descriptors survive a rebuild from the project, under both laws (OS-BUG-PASTE24)") {
  for (const unsigned law : {0u, 1u}) {
    CAPTURE(law);
    o3os::model::ModelArena& model = o3os::test::og::arena(0);
    model.project.reset();
    model.project.set_volume_law(static_cast<std::uint8_t>(law));
    LawRig rig(model);
    NEED(o3os::app::install_demo_kit(rig.bank, *rig.voices, model.project, rig.arena.data(), static_cast<std::uint32_t>(rig.arena.size())));
    CHECK(rig.bank.demo_kit());
    const o3os::audio::LevelLaw want = law != 0u ? o3os::audio::LevelLaw::Og1114 : o3os::audio::LevelLaw::Og12;
    SampleDesc kit[4];
    for (unsigned i = 0; i < 4; ++i) {
      kit[i] = rig.voices->sample(i);
      CHECK(kit[i].gain == 65536);  // the kit is at unity
      const o3os::model::Instrument* in = model.project.instrument(i);
      NEED(in != nullptr);
      CHECK(o3os::audio::level_gain_q16(in->sampler.volume, want) == 65536);  // and so is what the instrument says
    }
    NEED(rig.rebuild_all());
    for (unsigned i = 0; i < 4; ++i) {
      CAPTURE(i);
      const SampleDesc rebuilt = rig.voices->sample(i);
      CHECK(rebuilt.gain == 65536);
      CHECK(rebuilt.length == kit[i].length);
      CHECK(rebuilt.data == kit[i].data);
    }
    rig.bank.clear();
    CHECK_FALSE(rig.bank.demo_kit());
  }
}

TEST_CASE("og samples: a project with no sample instrument loads, gets the demo kit, and a rebuild of KICK is still 0 dB (OS-BUG-PASTE24)") {
  for (const unsigned version : {6u, 16u}) {
    CAPTURE(version);
    TempDir tmp;
    ProjectSpec ps;
    ps.version = version;
    write_file(tmp.path, "Workspace/project.mt", project_bytes(ps));  // a project with no instrument at all, as one of MIDI instruments only has no sample instrument
    DirStorage card(tmp.path.string());
    NEED(card.mount() == hs::Status::Ok);
    OgFolderLoader& pl = loader();
    o3os::model::ModelArena& model = o3os::test::og::arena(0);
    model.project.reset();
    NEED(pl.begin(card, "Workspace", model));
    NEED(run_loader(pl, 4096) == OgFolderLoader::State::Done);
    NEED(o3os::app::commit_og_law_if_done(model.project, pl));  // the platform's rule at the Done of the load
    CHECK(model.project.volume_law() == (version >= 7u ? 0u : 1u));
    LawRig rig(model);
    rig.bank.set_law(o3os::app::project_law(model.project));
    NEED(o3os::app::install_demo_kit(rig.bank, *rig.voices, model.project, rig.arena.data(), static_cast<std::uint32_t>(rig.arena.size())));
    const o3os::model::Instrument* kick = model.project.instrument(0);
    NEED(kick != nullptr);
    CHECK(std::strcmp(kick->name, "KICK") == 0);
    CHECK(kick->sampler.volume == (version >= 7u ? 50u : 100u));
    const SampleDesc loaded = rig.voices->sample(0);
    NEED(rig.rebuild_all());
    CHECK(rig.voices->sample(0).gain == loaded.gain);
    CHECK(rig.voices->sample(0).gain == 65536);
    char line[200];
    const std::size_t n = o3os::app::describe_instrument(0, kick, rig.voices->sample(0), line, sizeof line, o3os::app::project_law(model.project) == o3os::audio::LevelLaw::Og1114 ? 1114 : 12, rig.bank.demo_kit());
    const std::string text(line, n);
    CHECK(text.find("vol " + std::to_string(kick->sampler.volume) + " gain +0.0 dB") != std::string::npos);
    CHECK(text.find(version >= 7u ? " law 12" : " law 1114") != std::string::npos);
    CHECK(text.find(" demo") != std::string::npos);
  }
}

// OS-BUG-DEMOKIT: the kit goes only into a project it fits (no sample or module instrument, no instrument of any kind in slots 0..47). A project of module instruments (SID, TB-303, Chip, FM) or of
// MIDI instruments in 0..47 has no sample instrument, and the kit was written over its instruments 1-4 at boot (a later save made the loss permanent). Only MIDI instruments in 48..63 (every OG
// project holds the 16 channels there) do not stop the kit and are kept. Pattern 1 and the tempo are the kit's only when the kit is installed and pattern 1 was empty.
TEST_CASE("og samples: the demo kit only goes where it overwrites no instrument and the project has no sample or module instrument (OS-BUG-DEMOKIT)") {
  using o3os::model::Instrument;
  using o3os::model::InstrumentKind;
  struct Put {
    unsigned slot;
    InstrumentKind kind;
  };
  struct Case {
    const char* what;
    std::vector<Put> have;
    bool kit;
  };
  const Case cases[] = {
      {"empty", {}, true},
      {"modules only", {{0, InstrumentKind::Module}, {1, InstrumentKind::Module}, {2, InstrumentKind::Module}, {3, InstrumentKind::Module}}, false},
      {"one module", {{5, InstrumentKind::Module}}, false},
      {"MIDI only", {{0, InstrumentKind::Midi}, {1, InstrumentKind::Midi}}, false},
      {"MIDI in slot 4 (outside the kit's slots, inside 0..47)", {{4, InstrumentKind::Midi}}, false},
      {"MIDI in slot 5", {{5, InstrumentKind::Midi}}, false},
      {"MIDI in slot 47, the last non-channel slot", {{47, InstrumentKind::Midi}}, false},
      {"MIDI in slot 47 and the channels", {{47, InstrumentKind::Midi}, {48, InstrumentKind::Midi}, {63, InstrumentKind::Midi}}, false},
      {"MIDI in slot 48 only, the first channel slot", {{48, InstrumentKind::Midi}}, true},
      {"MIDI channels only, as every OG project", {{48, InstrumentKind::Midi}, {49, InstrumentKind::Midi}, {63, InstrumentKind::Midi}}, true},
      {"modules and the MIDI channels", {{0, InstrumentKind::Module}, {48, InstrumentKind::Midi}}, false},
      {"one sample", {{0, InstrumentKind::Sample}}, false},
      {"one sample far from 1-4", {{20, InstrumentKind::Sample}}, false},
      {"one module far from 1-4", {{20, InstrumentKind::Module}}, false},
      {"mixed", {{0, InstrumentKind::Module}, {1, InstrumentKind::Midi}, {2, InstrumentKind::Sample}}, false},
  };
  for (unsigned run = 0; run < 2 * (sizeof cases / sizeof cases[0]); ++run) {
    const Case& c = cases[run / 2];
    const bool notes = (run & 1u) != 0;  // second pass of each case: pattern 1 already holds a note
    CAPTURE(c.what);
    CAPTURE(notes);
    o3os::model::ModelArena& model = o3os::test::og::arena(0);
    model.project.reset();
    constexpr std::uint32_t kTempo = 13700;  // not the kit's tempo
    NEED(model.project.set_tempo_x100(kTempo) == o3os::model::Status::Ok);
    if (notes) {
      o3os::model::Step s = o3os::model::kEmptyStep;
      s.note = 61;
      s.instr = 9;
      NEED(model.project.create_pattern(1, 16) == o3os::model::Status::Ok);
      NEED(model.project.set_step(1, 5, 7, s) == o3os::model::Status::Ok);
    }
    NEED(model.project.add_module("tb303", 5, 1, 0) == o3os::model::Status::Ok);
    std::vector<Instrument> before;
    for (const Put& p : c.have) {
      Instrument in;
      o3os::model::init_instrument(in, p.kind);
      std::snprintf(in.name, sizeof in.name, "X%u", p.slot);
      if (p.kind == InstrumentKind::Midi) {
        in.midi_channel = 1;
      }
      if (p.kind == InstrumentKind::Module) {
        in.module_index = 0;
      }
      NEED(model.project.set_instrument(p.slot, in) == o3os::model::Status::Ok);
      before.push_back(in);
    }
    LawRig rig(model);
    CHECK(o3os::app::install_demo_kit(rig.bank, *rig.voices, model.project, rig.arena.data(), static_cast<std::uint32_t>(rig.arena.size())) == c.kit);
    CHECK(rig.bank.demo_kit() == c.kit);
    CHECK(rig.voices->has_samples() == c.kit);
    if (c.kit) {
      NEED(model.project.instrument(0) != nullptr);
      CHECK(std::strcmp(model.project.instrument(0)->name, "KICK") == 0);
    }
    CHECK(model.project.instrument_count() == before.size() + (c.kit ? 4u : 0u));
    // Pattern 1 and the tempo: the kit's own only when it was installed into an empty pattern 1; untouched when the kit was refused or pattern 1 already had a note.
    if (c.kit && !notes) {
      CHECK_FALSE(o3os::model::demo::pattern_is_empty(model.project, 1));
      CHECK(model.project.tempo_x100() != kTempo);
    } else {
      CHECK(model.project.tempo_x100() == kTempo);
      if (notes) {
        NEED(model.project.pattern(1) != nullptr);
        const o3os::model::Step& s = model.project.pattern(1)->steps[5][7];
        CHECK(s.note == 61);
        CHECK(s.instr == 9);
        CHECK(model.project.pattern(1)->steps[0][0].note == o3os::model::kEmptyStep.note);  // no kick of the kit's pattern
      } else {
        CHECK(o3os::model::demo::pattern_is_empty(model.project, 1));
      }
    }
    for (std::size_t i = 0; i < before.size(); ++i) {  // what the project had is as it was, with the kit or without
      const Instrument* now = model.project.instrument(c.have[i].slot);
      NEED(now != nullptr);
      CHECK(now->kind == before[i].kind);
      CHECK(std::strcmp(now->name, before[i].name) == 0);
    }
  }
}

TEST_CASE("og samples: the document's law: a rebuild reads it, reset puts it back, KEEP INSTRUMENTS keeps it, a failed load commits nothing (OS-BUG-PASTE24)") {
  o3os::model::ModelArena& model = o3os::test::og::arena(0);
  model.project.reset();
  LawRig rig(model);
  NEED(o3os::app::install_demo_kit(rig.bank, *rig.voices, model.project, rig.arena.data(), static_cast<std::uint32_t>(rig.arena.size())));
  // instrument 1 at VOLUME 100
  o3os::model::Instrument loud = *model.project.instrument(0);
  loud.sampler.volume = 100;
  NEED(model.project.set_instrument(0, loud) == o3os::model::Status::Ok);
  // a rebuild reads the document's law, whatever the bank last held
  rig.bank.set_law(o3os::audio::LevelLaw::Og1114);  // a stale copy
  NEED(rig.rebuild_all());
  CHECK(rig.bank.law() == o3os::audio::LevelLaw::Og12);
  CHECK(rig.voices->law() == o3os::audio::LevelLaw::Og12);
  CHECK(rig.voices->sample(0).gain == o3os::audio::level_gain_q16(100, o3os::audio::LevelLaw::Og12));
  model.project.set_volume_law(1);
  NEED(rig.rebuild_all());
  CHECK(rig.bank.law() == o3os::audio::LevelLaw::Og1114);
  CHECK(rig.voices->law() == o3os::audio::LevelLaw::Og1114);
  CHECK(rig.voices->sample(0).gain == 65536);
  // reset: a new document is the 1.2+ law; KEEP INSTRUMENTS keeps the instruments and with them the law
  model.project.reset(true);
  CHECK(model.project.volume_law() == 1u);
  model.project.reset(false);
  CHECK(model.project.volume_law() == 0u);
  // a load that does not reach Done commits nothing: a v6 document, then a folder that is no project (it fails before the project reader resets the document; a project.mt that fails to parse, after
  // that reset, leaves an empty document at the 1.2+ law)
  {
    TempDir tmp;
    write_vol100_project(tmp.path, 6u);
    DirStorage card(tmp.path.string());
    NEED(card.mount() == hs::Status::Ok);
    OgFolderLoader& pl = loader();
    NEED(pl.begin(card, "Workspace", model));
    NEED(run_loader(pl, 4096) == OgFolderLoader::State::Done);
    NEED(o3os::app::commit_og_law_if_done(model.project, pl));
    CHECK(model.project.volume_law() == 1u);
    const bool began = pl.begin(card, "NoSuchFolder", model);
    const OgFolderLoader::State end = began ? run_loader(pl, 4096) : OgFolderLoader::State::Failed;
    CHECK(end != OgFolderLoader::State::Done);
    CHECK_FALSE(o3os::app::commit_og_law_if_done(model.project, pl));
    if (model.project.instrument(0) != nullptr) {  // the document survived: it is read through the law it was loaded with
      CHECK(model.project.volume_law() == 1u);
    }
  }
  // the order native, OG v6, native: each load sets the law of its own file
  {
    std::vector<std::uint8_t> bytes;
    o3os::model::ModelArena& other = o3os::test::arena(2);
    other.project.reset();
    NEED(o3os::test::save(other.project, bytes) == o3os::formats::native::Progress::Done);
    NEED(o3os::test::load(model, bytes) == o3os::formats::native::Progress::Done);
    CHECK(model.project.volume_law() == 0u);
    TempDir tmp;
    write_vol100_project(tmp.path, 6u);
    DirStorage card(tmp.path.string());
    NEED(card.mount() == hs::Status::Ok);
    OgFolderLoader& pl = loader();
    NEED(pl.begin(card, "Workspace", model));
    NEED(run_loader(pl, 4096) == OgFolderLoader::State::Done);
    NEED(o3os::app::commit_og_law_if_done(model.project, pl));
    CHECK(model.project.volume_law() == 1u);
    NEED(o3os::test::load(model, bytes) == o3os::formats::native::Progress::Done);
    CHECK(model.project.volume_law() == 0u);
  }
}

// D5: the line ends with the law on every branch, says the demo kit, and names a copy of the law that differs from the document's.
TEST_CASE("og samples: `diag inst` ends with the law on every branch, and names a drifted copy (OS-BUG-PASTE24)") {
  o3os::model::Instrument midi{};
  midi.kind = o3os::model::InstrumentKind::Midi;
  SampleDesc none;
  char line[240];
  auto text = [&](const o3os::model::Instrument* in, const SampleDesc& d, int law, bool demo, int bank, int voices) {
    const std::size_t n = o3os::app::describe_instrument(0, in, d, line, sizeof line, law, demo, bank, voices);
    return std::string(line, n);
  };
  CHECK(text(&midi, none, 12, false, 12, 12) == "inst 01: not a sample instrument law 12");
  o3os::model::Instrument smp = sample_instrument(0);
  const std::string unbound = text(&smp, none, 1114, true, 1114, 1114);
  CHECK(unbound.find(", silent: nothing is bound vol 50 gain ") != std::string::npos);  // E2: the level is there too
  CHECK(unbound.find(" dB law 1114 demo") != std::string::npos);
  static std::int16_t pcm[4096] = {};
  const SampleDesc d = o3os::app::make_sample_desc(smp, pcm, 4096, nullptr, nullptr, o3os::audio::LevelLaw::Og12);
  const std::string ok = text(&smp, d, 12, false, 12, 12);
  CHECK(ok.find(" law 12") != std::string::npos);
  CHECK(ok.find("BANK-LAW") == std::string::npos);
  CHECK(ok.find("VOICES-LAW") == std::string::npos);
  const std::string drift = text(&smp, d, 12, false, 1114, 1114);
  CHECK(drift.find(" law 12 BANK-LAW 1114 VOICES-LAW 1114") != std::string::npos);
  CHECK(text(&smp, d, -1, false, -1, -1).find(" law") == std::string::npos);  // callers that do not pass a law get the line as before
}

// E2: an instrument with no PCM bound (an OG fixture without its audio) says its level too, in the format of the bound line, so `diag inst` reads a level for every sample instrument.
TEST_CASE("og samples: `diag inst` of an instrument with nothing bound says vol and gain through the law (OS-BUG-PASTE24 E2)") {
  o3os::model::Instrument smp = sample_instrument(0);
  char line[240];
  auto text = [&](unsigned volume, int law) {
    smp.sampler.volume = static_cast<std::uint8_t>(volume);
    const std::size_t n = o3os::app::describe_instrument(2, &smp, SampleDesc{}, line, sizeof line, law);
    return std::string(line, n);
  };
  CHECK(text(100, 12) == "inst 03: mode 0, silent: nothing is bound vol 100 gain +24.0 dB law 12");
  CHECK(text(50, 12) == "inst 03: mode 0, silent: nothing is bound vol 50 gain +0.0 dB law 12");
  CHECK(text(37, 12) == "inst 03: mode 0, silent: nothing is bound vol 37 gain -6.2 dB law 12");
  CHECK(text(100, 1114) == "inst 03: mode 0, silent: nothing is bound vol 100 gain +0.0 dB law 1114");
  CHECK(text(0, 12) == "inst 03: mode 0, silent: nothing is bound vol 0 gain -inf dB law 12");
  // the bound line reads the same words: the two share " vol N gain X dB"
  static std::int16_t pcm[4096] = {};
  smp.sampler.volume = 37;
  const SampleDesc d = o3os::app::make_sample_desc(smp, pcm, 4096, nullptr, nullptr, o3os::audio::LevelLaw::Og12);
  const std::size_t n = o3os::app::describe_instrument(2, &smp, d, line, sizeof line, 12);
  CHECK(std::string(line, n).find(" vol 37 gain -6.2 dB ") != std::string::npos);
}

// The demo flag ends with any new audio in a slot.
TEST_CASE("og samples: new audio in a slot ends the demo kit's flag (OS-BUG-PASTE24)") {
  o3os::model::ModelArena& model = o3os::test::og::arena(0);
  model.project.reset();
  LawRig rig(model);
  NEED(o3os::app::install_demo_kit(rig.bank, *rig.voices, model.project, rig.arena.data(), static_cast<std::uint32_t>(rig.arena.size())));
  CHECK(rig.bank.demo_kit());
  NEED(rig.rebuild_all());  // a rebuild (a Params edit) is not new audio: the kit is still what is there
  CHECK(rig.bank.demo_kit());
  rig.bank.adopt(0, rig.arena.data() + 90000, 100);
  CHECK_FALSE(rig.bank.demo_kit());
}

