// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Loads an OG project folder (the Tracker OG's own layout, docs/formats/og.md section 1) from a hal::storage::Storage into the project model
// (OS-007e). The OG readers of formats/og do the parsing, one resumable reader per file; this is the part that finds the files, feeds the
// readers from the card and steps from one file to the next, in bounded slices so the main loop (and with it the screen) keeps running:
//
//   <folder>/project.mt                        og::ProjectReader      first: it resets the project; a failure here ends the load
//   <folder>/patterns/pattern_NN.mtp           og::PatternReader      every one the directory lists, in its order (the id comes from the name)
//   <folder>/instruments/instrument_NN.mti     og::InstrumentReader   a sample instrument's metadata (the slot is NN - 1)
//   <folder>/instruments/NN name.pti, instrument_NN.pti                its .pti sibling: the header only; a .pti beside an .mti of the same slot wins
//
// NOT loaded here, by decision (OS-007e): the sample PCM (a `.pti`'s frames, the `samples/instrNN.wav` an `.mti` points at): the report says
// how many frames the .pti files declare; loading them into the sample arena is og_samples.h's (OS-007f), which starts from this loader's
// result (pti_slots()); and `patterns/patternsMetadata` (the pattern names), which is not mentioned in the scope of this slice. A pattern or
// instrument file that fails (a bad CRC, a truncated file) is counted and the load goes on: the project is whatever could be read, and the
// report says what was not.
//
// READ ONLY: the only storage calls are open, read, size, close, opendir, readdir, closedir (hal/storage.h has no others).
//
// Contexts: main loop. One step() moves at most about `byte_budget` bytes and visits at most kMaxActionsPerStep directory entries. No heap; the
// readers (about 7 KB) are members, so the loader is a static of its owner.

#include <o3os/core/byte_io.h>
#include <o3os/formats/format.h>
#include <o3os/formats/og.h>
#include <o3os/hal/storage.h>
#include <o3os/model/project.h>

#include <cstddef>
#include <cstdint>

namespace o3os::app {

// A core::ByteSource over an open file of a Storage: End at the end of the file, Failed on a storage error. Counts the bytes it hands out.
class StorageSource final : public core::ByteSource {
 public:
  void attach(hal::storage::Storage& storage, hal::storage::FileHandle file) noexcept {
    storage_ = &storage;
    file_ = file;
  }
  void reset() noexcept {
    storage_ = nullptr;
    file_ = hal::storage::FileHandle{};
    total_ = 0;
    error_ = hal::storage::Status::Ok;
  }
  core::IoResult read(void* dst, std::size_t max) noexcept override;
  std::uint32_t total() const noexcept { return total_; }  // bytes handed out since reset() (wraps at 4 GiB: only differences are used)
  // The storage's answer to the read that failed (Ok while none did): a reader reports "the source failed" and nothing more, and a card that
  // fails is not a bad file.
  hal::storage::Status error() const noexcept { return error_; }
  void clear_error() noexcept { error_ = hal::storage::Status::Ok; }

 private:
  hal::storage::Storage* storage_ = nullptr;
  hal::storage::FileHandle file_{};
  std::uint32_t total_ = 0;
  hal::storage::Status error_ = hal::storage::Status::Ok;
};

struct OgLoadReport {
  bool project_ok = false;               // project.mt was read (and the project reset to it)
  std::uint8_t version = 0;              // its structure version
  std::uint8_t start_pattern = 0;        // the pattern the project was saved on (at least 1); 0 until the project was read
  bool unverified_version = false;       // newer than any with evidence: read as far as the layout is known (the UI tells the user)
  bool observed_only = false;           // v17: no file of it seen (open research question)
  std::uint32_t dropped_groups = 0;      // formats::og::DropGroup bits of what the model has no place for and was not kept
  std::uint32_t patterns = 0;            // patterns applied
  std::uint32_t patterns_failed = 0;
  std::uint32_t instruments = 0;         // instruments applied
  std::uint32_t instruments_failed = 0;
  std::uint32_t instruments_superseded = 0;  // an .mti skipped because a .pti of the same slot was there
  std::uint32_t pcm_frames = 0;          // frames the .pti files declare: not loaded by this loader (og_samples.h loads them)
  std::uint32_t files_ignored = 0;       // entries of patterns/ and instruments/ that are not files of those kinds
  std::uint32_t micromoves_dropped = 0;  // v6 micromove values with no FX column free for them, or with Random value 16 in the other one (OS-CEPAT A10)
  std::uint32_t values_clamped = 0;      // v6 volume values above 100, read as full volume
  std::uint32_t ce_fx_cleared = 0;       // OG FX ids 75..127, cleared to (none, 0) by the id map of OS-CEPAT 3.1
  std::uint32_t bytes_read = 0;
  formats::FormatError error = formats::FormatError::None;  // why the load failed, or the first file that did not read
  const char* detail = "";               // the reader's words for it, or the loader's ("NO PROJECT FILE")
  hal::storage::Status storage_error = hal::storage::Status::Ok;  // a card error that ended the load
};

class OgFolderLoader {
 public:
  enum class State : std::uint8_t { Idle, Running, Done, Failed };

  static constexpr std::size_t kFolderMax = 255;               // bytes of the folder path
  static constexpr unsigned kMaxActionsPerStep = 32;           // directory entries or file opens a step may do
  static constexpr std::uint32_t kMaxEntriesPerDirectory = 4096;  // a directory that lists more is cut (a bound, not a limit anyone reaches: OG has 255 patterns)

  // Starts loading `folder` (UTF-8, relative to the card's root, e.g. "Workspace" or "Projects/My Song") into `arena`'s project. False when the
  // folder is not a valid path or does not fit (nothing started). Anything running is abandoned (its files closed). `storage` and `arena`
  // must outlive the load.
  bool begin(hal::storage::Storage& storage, const char* folder, model::ModelArena& arena) noexcept;
  // Does a slice of the work. Returns the state after it: Running (call again), Done or Failed.
  State step(std::size_t byte_budget) noexcept;
  // Abandons a load and closes its files. The project is left as far as it got.
  void abort() noexcept;

  State state() const noexcept { return state_; }
  const OgLoadReport& report() const noexcept { return report_; }
  const char* folder() const noexcept { return base_; }
  // Bit n: a `.pti` was applied to slot n, so that slot's PCM is in that file (after its 392-byte head) and not in samples/instrNN.wav (OS-007f).
  std::uint64_t pti_slots() const noexcept { return pti_slots_; }

 private:
  enum class Stage : std::uint8_t {
    ProjectOpen, ProjectRead, PatternsOpen, PatternNext, InstrumentsOpen, InstrumentNext, FileRead, Finish
  };
  enum class Kind : std::uint8_t { Pattern, InstrumentMti, InstrumentPti };

  void advance(std::size_t room) noexcept;
  void open_directory(const char* sub, Stage when_found, Stage when_missing) noexcept;
  void next_entry(bool patterns, Stage after_listing) noexcept;
  bool start_file(Kind kind, const char* sub, const char* name, std::uint16_t index, std::uint32_t size) noexcept;
  void finish_file(formats::Progress progress) noexcept;
  bool join(const char* a, const char* b) noexcept;  // path_ = base_ [/ a [/ b]]
  void close_all() noexcept;
  void fail(formats::FormatError e, const char* detail) noexcept;
  void fail_storage(hal::storage::Status s) noexcept;
  void note_problem(formats::FormatError e, const char* detail) noexcept;

  hal::storage::Storage* storage_ = nullptr;
  model::ModelArena* arena_ = nullptr;
  State state_ = State::Idle;
  Stage stage_ = Stage::ProjectOpen;
  Kind kind_ = Kind::Pattern;            // the file being read
  std::uint16_t index_ = 0;              // its pattern id or instrument slot
  hal::storage::FileHandle file_{};
  hal::storage::DirHandle dir_{};
  std::uint32_t entries_ = 0;
  std::uint64_t pti_slots_ = 0;          // bit n: a .pti was applied to slot n
  OgLoadReport report_{};
  formats::FormatReader* reader_ = nullptr;
  StorageSource source_;
  char base_[kFolderMax + 1] = {};
  char path_[hal::storage::kPathMax + 1] = {};
  hal::storage::DirEntry entry_{};
  formats::og::ProjectReader project_;
  formats::og::PatternReader pattern_;
  formats::og::InstrumentReader mti_{false};
  formats::og::InstrumentReader pti_{true};
};

// One line that says what a load did, for the log and `diag sd`: "Workspace: v16, 12 patterns, 20 instruments", with ", 2 failed" when files did not
// read, ", unverified version" when the project file's layout is newer than any seen, and, for a v6 pattern that lost something (OS-CEPAT S2),
// ", 3 MICRO-MOVES NOT KEPT" and ", n VALUES CHANGED" (clamped volumes and cleared tracker-ce FX together); "Workspace: FAILED NO PROJECT FILE" when
// the load failed; "" when none has run. Written into `out` (at most `cap` - 1 characters, always terminated); returns its length. No printf: numbers by hand.
std::size_t describe_load(const OgFolderLoader& loader, char* out, std::size_t cap) noexcept;

}  // namespace o3os::app
