// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-CEPTFX S1: the catalogue of the `.ptfx` plugins on the card (docs/specs/OS-CEPTFX.md A14 and 3.1): a stepped scan over one folder ("Effects"),
// driven by a core::Job like the File screen's DirScan — one directory page or one 472-byte header read a step, with progress. Each header goes
// through ptfx_parse_header; no plugin code is loaded or run, and nothing is written: the storage calls are opendir, readdir, open, read, size,
// close only, so "/Effects" is never created (A14).
//
// What the scan keeps is the 3.1 list, per file: the file name, the display name, the flags, the status (ok, or the refusal — a damaged file is
// listed with its status, greyed, never refused the listing), the plugin's id (A7) and whether it is quarantined. That is 88 B a file, capped at
// 128 files and sorted by display name without case (A14). A file name over 47 characters is skipped and counted. A card error on a file is not a
// refusal either: the file is simply not listed and is retried at the next scan (3.1) — every begin() reads the card afresh.
//
// Where it lives: the entries array is the catalogue, 11 KiB, and belongs in the `ptfx` SDRAM region (3.2: offset 720 KiB of 16 KiB), taken on the
// MK66 through core_view() once S2 adds the region; the owner passes the pointer to the constructor, so nothing of the catalogue is in SRAM. The
// job's own few dozen bytes are the caller's, like any job's.
//
// Contexts: main loop only. No heap: the folder is a member, a step's DirEntry (260 bytes), path (512) and header buffer (472) are on the stack.

#pragma once

#include <o3os/core/job.h>
#include <o3os/hal/storage.h>
#include <o3os/module/ptfx_format.h>
#include <o3os/module/ptfx_quarantine.h>

#include <cstddef>
#include <cstdint>

namespace o3os::module {

inline constexpr std::uint32_t kPtfxCatalogMax = 128;  // A14: up to 128 files (verified parity)
inline constexpr std::size_t kPtfxFileNameMax = 47;    // A14: a file name over 47 characters is skipped and counted
inline constexpr std::size_t kPtfxEntryBytes = 88;     // 3.1: 88 B a file
inline constexpr std::size_t kPtfxCatalogBytes = kPtfxCatalogMax * kPtfxEntryBytes;  // 11 KiB, in the `ptfx` region (3.2)

// One file of the catalogue (3.1). 88 bytes.
struct PtfxEntry {
  char file_name[kPtfxFileNameMax + 1];  // as the card listed it
  char display_name[33];                 // the header's name; the file name when the refusal came before the name (NOT A PTFX PLUGIN)
  std::uint16_t id;                      // A7: 0x8000 | the 15-bit FNV-1a of "ptfx:" + file name
  std::uint16_t flags;
  PtfxStatus status;  // Ok, or the refusal: the file is listed with it
  bool quarantined;   // A7: the plugin is in the boot's PtfxQuarantine (S2c): stamped by a scan that has the set attached, or by quarantine(id) later
};
static_assert(sizeof(PtfxEntry) == kPtfxEntryBytes, "3.1: 88 B a file, 11 KiB for 128 files");

// A7: the plugin's id, `0x8000 |` a 15-bit FNV-1a hash of `"ptfx:" + file name` (a namespace distinct from the `.tmod` stems).
std::uint16_t ptfx_plugin_id(const char* file_name) noexcept;

class PtfxCatalog final : public core::Job {
 public:
  enum class State : std::uint8_t {
    Idle,       // nothing asked yet, or invalidated: the owner starts it
    Scanning,   // the job is running
    Done,       // the whole folder was read
    NoCard,     // the card is not there or not mounted
    NoFolder,   // the folder is not on the card
    Failed,     // the card or the bus failed on the directory (error())
    Cancelled,  // cancel(): what was read stays listed
  };

  static constexpr std::size_t kFolderMax = 255;           // bytes of the folder path
  static constexpr std::uint32_t kUnitUs = 250;            // microseconds of budget a unit buys (PROVISIONAL, as DirScan's)
  static constexpr unsigned kMaxUnitsPerStep = 32;         // a step is bounded whatever budget it is handed
  // What one 472-byte header read (open, size, read, close) costs in units. 3.1: a step is one directory page OR one header read, so this is
  // the step's cap: after one header read the step is over whatever budget it had left (at most one header read a step).
  static constexpr unsigned kHeaderReadUnits = kMaxUnitsPerStep;

  // `entries` must hold kPtfxCatalogMax entries and outlive the scan: it IS the catalogue, placed by the owner in the `ptfx` SDRAM region
  // (3.2, offset 720 KiB of 16 KiB). Not owned, not cleared beyond count(): a new begin() overwrites from 0.
  explicit PtfxCatalog(PtfxEntry* entries) noexcept;

  enum class Begin : std::uint8_t {
    Started,  // state is Scanning: hand this job to the runner
    BadPath,  // not a valid folder path, or too long: nothing changed
  };
  // Targets `folder` (default "Effects", A14; UTF-8, relative to the card's root, a leading or trailing '/' ignored). A scan that is running is
  // abandoned (its directory closed) and replaced; a Done one is scanned again — every begin() reads the card afresh, which is what retries a
  // file whose read hit a card error. `storage` must outlive the scan.
  Begin begin(hal::storage::Storage& storage, const char* folder = "Effects") noexcept;
  // Forgets the listing and the folder: closes the directory, State::Idle.
  void invalidate() noexcept;
  // Closes the directory without touching the state: for an owner that is going away.
  void abandon() noexcept;

  core::JobStatus step(std::uint32_t budget_us) noexcept override;
  // The folder's length is not known in advance: the directory entries read so far against the cap; kJobProgressMax when Done.
  unsigned progress() const noexcept override;
  void cancel() noexcept override;
  const char* name() const noexcept override { return "PTFX SCAN"; }
  const char* detail() const noexcept override;

  State state() const noexcept { return state_; }
  unsigned count() const noexcept { return count_; }  // files listed, at most kPtfxCatalogMax, sorted
  const PtfxEntry& entry(unsigned i) const noexcept { return entries_[i]; }
  // More .ptfx files than the cap: the ones that sort after the last kept are dropped (truncated).
  bool truncated() const noexcept { return overflow_ != 0u; }
  unsigned skipped_long() const noexcept { return skipped_long_; }    // A14: file names over 47 characters
  unsigned skipped_other() const noexcept { return skipped_other_; }  // not a .ptfx file, hidden, a folder, gone before the open
  // 3.1: a card error is not a refusal: the file is not listed and is retried at the next scan. A scan can end Done with card_errors() > 0
  // (the error was on files, not on the folder): the owner must check this and say that some files are missing until the next scan.
  unsigned card_errors() const noexcept { return card_errors_; }
  // The storage's answer that ended the scan (Ok while none did).
  hal::storage::Status error() const noexcept { return error_; }

  // A14's order: by display name without case, ties by file name, likewise. For the tests.
  static int compare_names(const PtfxEntry& a, const PtfxEntry& b) noexcept;
  // An ASCII case-insensitive "ends with", with a stem before the tail (".ptfx" alone is no plugin).
  static bool is_ptfx_name(const char* name) noexcept;
  // A7: marks the entry with this id quarantined (a crash record named it). False when no listed entry has it.
  bool quarantine(std::uint16_t id) noexcept;
  // S2c: the boot's quarantine set (not owned, must outlive the catalogue; null for none). A scan lists an entry quarantined when the set holds its id.
  void set_quarantine(const PtfxQuarantine* set) noexcept { qset_ = set; }

 private:
  void close_dir() noexcept;
  void end(State state, hal::storage::Status status) noexcept;
  // One directory entry read: listed, skipped or read as a header; returns what it cost in units (1).
  unsigned take(const hal::storage::DirEntry& entry) noexcept;
  // The sorted insert of one entry; drops it (overflow_) when the list is full and it sorts after the last kept.
  void insert(const PtfxEntry& e) noexcept;

  hal::storage::Storage* storage_ = nullptr;
  PtfxEntry* entries_;
  const PtfxQuarantine* qset_ = nullptr;
  hal::storage::DirHandle dir_{};
  State state_ = State::Idle;
  hal::storage::Status error_ = hal::storage::Status::Ok;
  unsigned count_ = 0;
  unsigned seen_ = 0;           // directory entries read (progress)
  unsigned overflow_ = 0;       // .ptfx files dropped because the cap was reached
  unsigned skipped_long_ = 0;
  unsigned skipped_other_ = 0;
  unsigned card_errors_ = 0;
  char folder_[kFolderMax + 1] = {};
};

}  // namespace o3os::module
