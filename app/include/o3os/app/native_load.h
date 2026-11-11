// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Loads a native project folder (OS-007o): <folder>/project.o3pj, the native format (formats/native.h, docs/formats/o3-project.md; the reader is the one the host tools and
// the tests use: nothing here parses) from a hal::storage::Storage into the project model. The counterpart of OgFolderLoader for the folders this OS saves (File's SAVE and
// SAVE AS write them); the platform runs the same phases after it (the settle before, the sample PCM and the SD modules after), so the sample files the instruments name
// (the sample paths of the file, relative to the folder: "samples/instr01.wav") are loaded by OgSampleLoader with no .pti slots.
//
// `backup` opens <folder>/project.o3pj.bak instead: what a power cut in the replacing save of OS-007n leaves when it ran between the two renames of its steps 8 and 9 (see
// atomic_save.h): the old version, whole, under the backup's name. The load is the same; the platform tells the user it is a recovery.
//
// READ ONLY: open, read, size, close. Main loop. One step() reads at most about `byte_budget` bytes. No heap; the reader (3 KB) is a member, so the loader is an object of its
// owner (the platform puts it in SDRAM, with the save layer).
//
// Failure leaves the document as native::Reader does: a project file that is read badly leaves the project reset (empty and valid), never half loaded; a folder with no file
// (or a card that fails to open it) leaves the document untouched.

#include <o3os/app/og_folder.h>
#include <o3os/formats/native.h>
#include <o3os/hal/storage.h>
#include <o3os/model/project.h>

#include <cstddef>
#include <cstdint>

namespace o3os::app {

struct NativeLoadReport {
  bool backup = false;                 // the file read is project.o3pj.bak
  formats::native::Error error = formats::native::Error::None;
  const char* detail = "";             // the reader's words for the error, or the loader's ("NO PROJECT FILE"); "" when nothing was wrong
  std::uint32_t bytes_read = 0;
  std::uint32_t patterns = 0;
  std::uint32_t instruments = 0;
  std::uint32_t unknown_dropped = 0;   // chunks of a newer writer that did not fit the pool
  bool newer_minor = false;            // a chunk with fields this reader does not know: they are lost on the next save
  std::uint32_t mevt_skipped = 0;      // MIDI take lanes the reader did not take (OS-MIDIREC-P1: a pattern the project lacks, an out-of-range lane...): lost on the next save
  std::uint32_t sanitised = 0;         // settings and session-state values that were out of range and read as their defaults (OS-PERSIST2)
  hal::storage::Status storage_error = hal::storage::Status::Ok;  // a card error that ended the load
};

class NativeFolderLoader {
 public:
  enum class State : std::uint8_t { Idle, Running, Done, Failed };

  // Starts loading `folder` (UTF-8, relative to the card's root, "Projects/My Song") into `arena`'s project. False when the folder is not a valid path or does not fit (nothing
  // started). Anything running is abandoned (its file closed). `storage` and `arena` must outlive the load.
  bool begin(hal::storage::Storage& storage, const char* folder, model::ModelArena& arena, bool backup = false) noexcept;
  // A new project instead of a file (File F3 NEW): the first step() resets `arena`'s project to the model's defaults (pattern 1 with kDefaultPatternLength steps, one song position
  // that plays it, 12 tracks: ADR 0003) and ends Done, with folder() "" (no folder holds it). The platform then runs the phases after a load. Anything
  // running is abandoned, as begin(). `keep_instruments` (OS-NEWKEEP, File > NEW > KEEP INSTRUMENTS): the instrument table and the module table the instruments name are left as they are.
  void begin_fresh(model::ModelArena& arena, bool keep_instruments = false) noexcept;
  // OS-MIDIREC-P1 S2: the document's MIDI take lanes. A file's MEVT chunk is read into them; a new project empties them. Without (nullptr, the default) MEVT is an unknown chunk.
  void attach_lanes(model::LaneStore* lanes) noexcept { lanes_ = lanes; }
  bool fresh() const noexcept { return fresh_; }
  bool keeps_instruments() const noexcept { return fresh_ && keep_instruments_; }  // the fresh project kept the instrument table (and so its audio)
  State step(std::size_t byte_budget) noexcept;
  void abort() noexcept;

  State state() const noexcept { return state_; }
  const NativeLoadReport& report() const noexcept { return report_; }
  const char* folder() const noexcept { return base_; }
  // The last load that was started was this loader's (the platform asks which loader's report to read: the OG one or this).
  bool active() const noexcept { return active_; }
  void release() noexcept { active_ = false; }

 private:
  void close_file() noexcept;
  void fail(formats::native::Error e, const char* detail) noexcept;

  hal::storage::Storage* storage_ = nullptr;
  model::ModelArena* arena_ = nullptr;
  model::LaneStore* lanes_ = nullptr;
  State state_ = State::Idle;
  bool active_ = false;
  bool fresh_ = false;    // begin_fresh(): no file
  bool keep_instruments_ = false;  // begin_fresh(): the instrument table survives the reset (OS-NEWKEEP)
  bool reading_ = false;  // the file is open and the reader has begun
  hal::storage::FileHandle file_{};
  NativeLoadReport report_{};
  StorageSource source_;
  char base_[OgFolderLoader::kFolderMax + 1] = {};
  formats::native::Reader reader_;
};

// OS-QAFIX-4 (finding 4, the load's first phase): before a project open replaces anything, the file is checked, so that one that cannot load leaves the document, the transport, the module
// instruments and the history as they are. A native folder: project.o3pj (or .bak) streamed through formats::native::Verifier (framing, versions, counts, CRC); an OG folder: project.mt opened
// and its size the Reader's length for the version in its header (each OG version has its own: formats/src/og_codec.h). READ ONLY, main loop, sliced like the load (step() reads at most `byte_budget` bytes), no heap; an object of its
// owner (about 1 KB). It never touches the arena. What it cannot see (a field out of range inside a chunk that has a good CRC, an OG file of the right size with bad fields) still fails in the
// loader's first slice, which resets the document as before. A file that changes between the verify and the load is not covered (saves and the USB hand-over are held out while a load phase runs).
class ProjectVerify {
 public:
  enum class State : std::uint8_t { Idle, Running, Ok, Refused };

  // `og` selects the OG folder check; `backup` the project.o3pj.bak of a native folder. False when `folder` is not a valid path or does not fit (nothing started). Anything running is closed.
  bool begin(hal::storage::Storage& storage, const char* folder, bool og, bool backup) noexcept;
  State step(std::size_t byte_budget) noexcept;
  void abort() noexcept;

  State state() const noexcept { return state_; }
  const char* detail() const noexcept { return detail_; }  // when Refused: why, in words ("THE FILE ENDS BEFORE ITS FORM DOES"; "NO PROJECT FILE")
  const char* folder() const noexcept { return base_; }
  bool og() const noexcept { return og_; }
  bool backup() const noexcept { return backup_; }

 private:
  void close_file() noexcept;
  void refuse(const char* detail) noexcept;

  hal::storage::Storage* storage_ = nullptr;
  State state_ = State::Idle;
  bool og_ = false;
  bool backup_ = false;
  bool reading_ = false;
  const char* detail_ = "";
  hal::storage::FileHandle file_{};
  StorageSource source_;
  char base_[OgFolderLoader::kFolderMax + 1] = {};
  formats::native::Verifier verifier_;
};

// One line for the log and `diag sd`: "Projects/Song: native, 12 patterns, 20 instruments" (", from the backup" for a recovery, ", newer fields dropped" ...); "...: FAILED NO PROJECT
// FILE" when it failed; "" when none has run. Same rules as describe_load: at most `cap` - 1 characters, terminated; returns the length.
std::size_t describe_native_load(const NativeFolderLoader& loader, char* out, std::size_t cap) noexcept;

}  // namespace o3os::app
