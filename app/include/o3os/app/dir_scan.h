// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Reads one folder of the card into a DirIndex, a few entries per main-loop pass (OS-010a). It is a core::Job (core/job.h): the runner gives
// it a slice of the pass, it does about that much storage work and returns, so a folder of hundreds of entries never stalls the screen, and
// the listing, sorted as it grows, is usable (and scrollable) before the scan has ended. READ ONLY: the storage calls are opendir, readdir,
// closedir and exists (hal/storage.h has no others).
//
// What is listed is the filter's (ListFilter): folders of a kind (every folder, or only those that hold a given file: an OG project is a
// folder with a project.mt in it, a native one a folder with a project.o3pj; the entry is tagged with which) and files of a kind (a name ending in ".wav", or none). The kind of an entry is what the card says it is
// (DirEntry::is_dir), never what its name looks like: a folder called "kick.wav" is a folder (PAR-033: it must not be opened as
// a file), and a file called "Projects" is not one. Hidden names (a leading '.', which is what macOS leaves: "._x", ".DS_Store") are not
// listed, nor is anything that fails the probe.
//
// OS-LOADCH: with ListFilter::tag_stereo a listed WAV or AIFF is also opened, and formats::sample_channels reads its header: the entry is marked stereo() when it has two channels. That is a probe too (kProbeUnits);
// a file that cannot be opened or read is simply not tagged.
//
// Cost model. A readdir is mostly a lookup in the sector the card already gave (a fraction of a millisecond), a probe is a path walk of three
// directory levels (a few milliseconds on a slow card). A step turns its microseconds into units, 1 for a readdir and kProbeUnits for a probe
// (kUnitUs a unit; PROVISIONAL until the hardware session, JobStats::max_step_us says what a step really cost), and stops when they are
// spent: at least one entry a step (so a scan always moves), at most kMaxUnitsPerStep units (so a step is bounded whatever budget it is
// handed). The default 2 ms pass is about one probe, or a few dozen plain entries.
//
// A scan that is Done is a cache: begin() for the same folder and filter says so and reads nothing; invalidate() throws it away (RESCAN, and
// the retry of a failed one when the screen is left and entered again).
//
// Handles. One directory is open while Scanning and no longer: Done, a failure, cancel() and abandon() close it. A card that was remounted
// under a scan answers BadHandle: the scan fails and does not close what is no longer its own.
//
// Contexts: main loop only. No heap; the folder is a member, a step's DirEntry (260 bytes) and probe path (512) are on the stack.

#include <o3os/app/dir_index.h>
#include <o3os/core/job.h>
#include <o3os/hal/storage.h>

#include <cstddef>
#include <cstdint>

namespace o3os::app {

struct ListFilter {
  const char* extension;  // ".wav": a file is listed when its name ends with it (ASCII case-insensitive); several, separated by spaces, are all taken (OS-SAMPFMT); nullptr: no file is listed
  const char* probe;      // "project.mt": a folder is listed only when it holds this file; nullptr: every folder is listed (to be entered)
  bool skip_hidden;       // names that start with '.' are not listed
  const char* native_probe = nullptr;   // "project.o3pj": a folder that holds it is listed too, tagged EntryTag::Native (checked first: the native file is the newer one); `probe` then tags Og
  const char* recover_probe = nullptr;  // "project.o3pj.bak": a folder that holds it and no native_probe file is tagged EntryTag::Recover (checked second): see app/atomic_save.h, "what a power cut leaves"
  const char* extension2 = nullptr;     // OS-GRAN: a second extension listed as well (".wt", Surge wavetables, in the Sample load screen)
  bool tag_stereo = false;              // OS-LOADCH: a listed .wav / .aif / .aiff / .aifc is opened and its header read (a probe's cost) to tag the stereo ones (DirIndex::stereo)
  const char* skip_probe = nullptr;     // OS-FILES part B: ".deleted": a folder that holds this file is not listed (checked before the others: DELETE marks a project folder with it)
  bool module_names = false;            // OS-IMPORT-ANY I2: a file whose name says a module (module_name_kind: the prefix names, "mod.title"; the extensions are `extension`'s) is listed as well
  bool all_files = false;               // OS-IMPORT-ANY I1: every file is listed (the IMPORT page's ALL FILES mode)
};

// OS-IMPORT-ANY I2: what a file name says about a module. The extension (".mod", ".s3m", ".med", ".mmd0" .. ".mmd3") or, when there is none of those, the prefix the Amiga programs put before the title
// ("mod.title", "MED.title", "s3m.title", "mmd1.title"), any case; None when the name says nothing. The bytes of the file have the last word (ModuleImport::begin): this is what a listing may know without reading.
enum class ModuleName : std::uint8_t { None, Mod, S3m, Med };
ModuleName module_name_kind(const char* name) noexcept;

// The File screen's: the folders that hold a project.o3pj (a native project, OS-004a), only a project.o3pj.bak (a native project whose save was cut between its two renames: RECOVER,
// OS-007o) or a project.mt (an OG project, docs/formats/og.md section 1).
inline constexpr ListFilter kProjectFilter = {nullptr, "project.mt", true, "project.o3pj", "project.o3pj.bak", nullptr, false, ".deleted"};
// The Sample load screen's, when it comes: every folder, and the .wav files.
inline constexpr ListFilter kWavFilter = {".wav .aif .aiff .aifc .8svx .svx .iff", nullptr, true, nullptr, nullptr, ".wt", true};  // (OS-GRAN: the .wt files of Surge are samples too; OS-SAMPFMT: so are AIFF and 8SVX)

// The Sample load screen's when the type chooser's LOAD opened it for an instrument file (OS-INSTUI S7): every folder, and the OG's .pti files.
inline constexpr ListFilter kPtiFilter = {".pti", nullptr, true};

// The File screen's IMPORT page (OS-FILES part A): every folder (to be entered) and the module files the importers read (OS-MODIMP, OS-008b): ProTracker .mod, Scream Tracker .s3m, OctaMED .med and .mmd0..3,
// and (OS-IMPORT-ANY I2) the prefix names of the Amiga programs ("mod.title").
inline constexpr ListFilter kModuleFilter = {".mod .s3m .med .mmd0 .mmd1 .mmd2 .mmd3", nullptr, true, nullptr, nullptr, nullptr, false, nullptr, true};
// The same page in its ALL FILES mode (OS-IMPORT-ANY I1): every folder and every file (hidden names still not), for modules that carry no type in their name.
inline constexpr ListFilter kAllFilesFilter = {nullptr, nullptr, true, nullptr, nullptr, nullptr, false, nullptr, false, true};

// OS-FILES part B: does `folder` ("Projects/<name>") hold the DELETE marker, an empty FILE called .deleted? A folder of that name is not a marker (it is a folder of the user's: it is listed, it opens, and DELETE
// on its project fails with "name exists"). The one question every project load asks before it starts: the File screen's open, a RECENT row, `diag load`, the boot (platform/mk66 start_project_load), so
// that a hidden project is never loaded, and a SAVE never writes into it.
bool folder_marked_deleted(hal::storage::Storage& storage, const char* folder) noexcept;

class DirScan final : public core::Job {
 public:
  enum class State : std::uint8_t {
    Idle,       // nothing asked yet, or invalidated: the owner starts it
    Scanning,   // the job is running
    Done,       // the whole folder was read (or the list filled up: index().full())
    NoCard,     // the card is not there or not mounted
    NoFolder,   // the folder is not on the card
    Failed,     // the card or the bus failed (error())
    Cancelled,  // cancel(): what was read stays listed
  };

  static constexpr std::size_t kFolderMax = 255;           // bytes of the folder path
  static constexpr std::uint32_t kUnitUs = 250;            // microseconds of budget a unit buys (PROVISIONAL)
  static constexpr unsigned kProbeUnits = 8;               // a probe costs this many readdirs (PROVISIONAL)
  static constexpr unsigned kMaxUnitsPerStep = 32;

  enum class Begin : std::uint8_t {
    Started,  // state is Scanning: hand this job to the runner
    Cached,   // the listing of this folder and filter is already here (Done): nothing to run
    BadPath,  // not a valid folder path, or too long: nothing changed
  };
  // Targets `folder` (UTF-8, relative to the card's root, "" or "/" for the root; a leading or trailing '/' is ignored) with `filter`. A scan
  // that is running is abandoned (its directory closed) and replaced. `storage`, the filter and its strings must outlive the scan (and the cache it leaves: a Done scan is compared with
  // the next begin()'s filter).
  Begin begin(hal::storage::Storage& storage, const char* folder, const ListFilter& filter) noexcept;
  // Forgets the listing and the folder: closes the directory, State::Idle. The owner releases the job's slot.
  void invalidate() noexcept;
  // Closes the directory without touching the state: for an owner that is going away (App's destructor).
  void abandon() noexcept;

  core::JobStatus step(std::uint32_t budget_us) noexcept override;
  // A folder's length is not known in advance: 0 while scanning, kJobProgressMax when it ended well.
  unsigned progress() const noexcept override { return state_ == State::Done ? core::kJobProgressMax : 0u; }
  void cancel() noexcept override;
  const char* name() const noexcept override { return "SCAN"; }
  const char* detail() const noexcept override;

  State state() const noexcept { return state_; }
  const DirIndex& index() const noexcept { return index_; }
  const char* folder() const noexcept { return folder_; }
  // The storage's answer that ended the scan (Ok while none did; NotFound, NoCard... for those states).
  hal::storage::Status error() const noexcept { return error_; }
  // Entries the filter looked at and did not list (hidden, wrong kind, no probe file, a path too long to probe).
  unsigned skipped() const noexcept { return skipped_; }
  // The scan stopped because the list filled up: there are more entries than the listing holds.
  bool truncated() const noexcept { return state_ == State::Done && index_.full(); }

  // The filter's verdicts, for the tests: an ASCII case-insensitive "ends with" (an empty `tail` matches nothing).
  static bool ends_with_ci(const char* name, const char* tail) noexcept;
  // ends_with_ci for any of the words of `list` (extensions separated by single spaces).
  static bool ends_with_any_ci(const char* name, const char* list) noexcept;

 private:
  void close_dir() noexcept;
  void end(State state, hal::storage::Status status) noexcept;
  // One entry read: listed, skipped or the end of the scan; `units` is what it cost.
  unsigned take(const hal::storage::DirEntry& entry) noexcept;
  // OS-LOADCH: does the file `entry` has two channels? (header probe: one open, a few small reads, one close; false when it cannot be opened or read)
  bool probe_stereo(const char* name) noexcept;

  hal::storage::Storage* storage_ = nullptr;
  const ListFilter* filter_ = nullptr;  // the caller's (a constant of the app: kProjectFilter, kWavFilter): a pointer, not a copy, so that App's block in SRAM_U does not grow with the filter's probes
  hal::storage::DirHandle dir_{};
  State state_ = State::Idle;
  hal::storage::Status error_ = hal::storage::Status::Ok;
  unsigned skipped_ = 0;
  DirIndex index_;
  char folder_[kFolderMax + 1] = {};
};

}  // namespace o3os::app
