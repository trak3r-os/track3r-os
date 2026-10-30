// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Imports a module file of the card (OS-FILES part A, docs/specs/OS-FILES-IMPORT.md; new Community OS behaviour) into the document: a ProTracker .mod or Scream Tracker 3 .s3m
// (formats/mod/import.h, OS-MODIMP) or an OctaMED .med / .mmd0..3 (formats/med/import.h, OS-008b). The importers are the registered readers of those formats; this is the loader around them,
// the counterpart of NativeFolderLoader and OgSampleLoader for the File screen's IMPORT page. The platform runs it as the phases of a project load (the settle before, the sample PCM and
// the SD modules after):
//
//   Project phase   ModuleImport::begin + step: the file is read through a random-access window over the card (an MMD module is a graph of offsets, and the MOD / S3M decoders go back and
//                   forth), the reader is stepped a bounded number of cells and card reads at a time, and the project is made. ORDER: begin() only checks the path, opens the file and records
//                   what the first step needs (the platform calls it BEFORE the settle hand-over, while the render still reads the document); the importer's first act, which resets the
//                   project, is the first step(), which runs only after Settle (the render has let the voices die and the transport is stopped), as the project loaders' writes do. A file that
//                   will not open (not found, a card error) fails at begin() and the document is as it was (document_touched() false). Any failure after that first step, a bad signature
//                   or a cut-off header included, leaves the document EMPTY (reset, valid): the platform then also empties the voices' sample table and the bank, which held the old PCM.
//   Samples phase   OgSampleLoader::begin_feed(import, ...): the importers do not write the PCM, they decode it on request (Importer::read_pcm: 8-bit times 256, 16-bit as it is, a stereo S3M
//                   sample its left channel), and ModuleImport is the PcmFeed that asks them. The samples go straight into the sample arena, bound to the voices at the module's own rate; they are
//                   on no card until the user saves (the bank does not mark them).
//   then            close(): the file is released, the sample counts go into the summary.
//
// The summary (ModuleSummary) is the import's report for the user: what was made, what was not kept, by name (the importers' drop groups), how many commands were approximated or dropped, and the
// reason when it failed. The File screen's REPORT page draws it (module_report_text).
//
// Contexts: main loop only. READ ONLY on the card (open, read, seek, size, close). No heap: the two importers (about 39 KB together) are members, so the object is a static of the platform in SDRAM.

#include <o3os/app/file_import.h>
#include <o3os/app/og_samples.h>
#include <o3os/formats/med/import.h>
#include <o3os/formats/mod/import.h>
#include <o3os/hal/storage.h>
#include <o3os/model/project.h>

#include <cstddef>
#include <cstdint>

namespace o3os::app {

struct ModuleSummary {
  enum class Format : std::uint8_t { None, Mod, S3m, Med };
  static constexpr std::size_t kNameChars = 24;

  bool valid = false;            // an import was started (the page has something to show)
  bool ok = false;               // the project was made
  Format format = Format::None;
  bool amiga_commands = false;   // the commands went to the engine window (Amiga engine)
  std::uint8_t channels = 0;     // MOD / S3M channels, MED tracks that carry data
  std::uint16_t patterns = 0;
  std::uint16_t order = 0;
  std::uint16_t tracks_kept = 0;
  std::uint16_t tracks_dropped = 0;
  std::uint16_t instruments = 0;       // made
  std::uint16_t instruments_refused = 0;
  std::uint16_t samples_loaded = 0;    // bound to a voice (the samples phase)
  std::uint16_t samples_no_room = 0;   // silent: the arena was full
  std::uint16_t samples_silent = 0;    // silent: no audio, unreadable
  std::uint32_t samples_kb = 0;
  std::uint32_t cmd_approx = 0;        // commands written as the nearest thing
  std::uint32_t cmd_dropped = 0;       // commands with no counterpart
  std::uint32_t clamped = 0;           // values the model could not hold
  std::uint32_t dropped_groups = 0;    // the importer's DropGroup bits (group_name says which)
  char name[kNameChars + 1] = {};      // the file's name, upper case, cut with '~'
  char error[48] = {};                 // the reason when !ok (upper case)
  const char* detail = "";             // the importer's own words for it (static text, lower case: "a pattern offset is past the end"), shown by the report

  // The names of the drop groups set (the format's own list).
  const char* group_name(unsigned bit) const noexcept;
};

// The report page's text: up to kLines lines of at most kChars characters (upper case), `count` of them used.
struct ModuleReportText {
  static constexpr unsigned kLines = 14;
  static constexpr unsigned kChars = 46;
  char line[kLines][kChars + 1];
  unsigned count = 0;
};
void module_report_text(const ModuleSummary& s, ModuleReportText& out) noexcept;

// The strip's line when the import ended: "IMPORTED KC_DANCINONAMIGA.MOD", "... : 3 NOT KEPT", "... 2 NO ROOM", or "IMPORT X.MOD FAILED: TRUNCATED". The name is cut, never the reason or counts.
void module_strip_text(const ModuleSummary& s, char* out, std::size_t cap) noexcept;

class ModuleImport final : public PcmFeed {
 public:
  enum class State : std::uint8_t { Idle, Running, Done, Failed };
  enum class Start : std::uint8_t { Started, BadPath, NotModule };
  // OS-IMPORT-ANY I3: what a file is, from its first bytes and its name. The name's word (extension or prefix, module_name_kind) must agree with the bytes (a MOD and an S3M are one reader's: either name takes
  // either); a name that says nothing is decided by the bytes. NotModule: no reader of this OS takes the file. Unreadable: it would not open or read (begin() then fails with its own words).
  enum class Probe : std::uint8_t { Mod, S3m, Med, NotModule, Unreadable };
  // Reads at most formats::kSniffBytes of `path` (UTF-8, relative to the card's root) and says what it is; a file the card does not give is Unreadable. READ ONLY, nothing else is touched: the File screen asks
  // it before the NEW question, so that a file that is no module never asks it. `path` that is not a valid path is NotModule.
  static Probe probe(hal::storage::Storage& storage, const char* path) noexcept;
  // The decision alone, from the bytes read (`head`, `n` of them) of a file of `size` bytes called `name` (a path or a bare name), for the tests.
  static Probe decide(const char* name, const std::uint8_t* head, std::size_t n, std::uint32_t size) noexcept;

  static constexpr std::size_t kMinCells = 256;   // a step always moves this much, whatever budget it was given
  static constexpr std::size_t kFillCells = 128;  // what one card read of the window costs in the step budget (a hostile MMD's jumping offsets seek backward on every cell)

  // Starts importing `path` (UTF-8, relative to the card's root, "MOD/song.mod") into `arena`'s project. The reader is chosen by the file's first bytes, through probe() (OS-IMPORT-ANY I3: MOD and S3M
  // by their signatures, OctaMED by MMD0..3; a name with an extension or a prefix that says otherwise is refused, a name that says nothing is decided by the bytes). BadPath: not a valid path; NotModule: no
  // module reader takes the file (nothing changed, the last import's report stays). A file that will not open is a Failed state at once (the document is untouched). Nothing is written to `arena` here: the reader starts at the first step().
  // `storage` and `arena` must outlive the import and the sample phase.
  Start begin(hal::storage::Storage& storage, const char* path, model::ModelArena& arena, ImportEngine engine) noexcept;
  // A slice of the work: about `byte_budget` / 8 cells (at least kMinCells) of the reader, a card read (a window fill) counting kFillCells. The first call resets the document.
  // Returns the state after it.
  State step(std::size_t byte_budget) noexcept;
  // The sample phase ended: its counts go into the summary, and the file is released.
  void samples_done(const SampleLoadReport& report) noexcept;
  // Releases the file (the PCM can no longer be read). The summary stays.
  void close() noexcept;

  State state() const noexcept { return state_; }
  const ModuleSummary& summary() const noexcept { return summary_; }
  // The last load that was started was this importer's (the platform asks which loader's report to read), as NativeFolderLoader::active().
  bool active() const noexcept { return active_; }
  void release() noexcept { active_ = false; }
  // The reader has begun: the document was reset, so it is the import's (a failure from here leaves it empty, and the platform empties what was bound to the old one).
  bool document_touched() const noexcept { return touched_; }

  // PcmFeed
  std::uint32_t frames(unsigned slot) const noexcept override;
  std::uint32_t rate(unsigned slot) const noexcept override;
  bool read(unsigned slot, std::uint32_t first, std::uint32_t n, std::int16_t* dst) noexcept override;

 private:
  // A random-access view of the file with a 512-byte window: the importers read a few bytes at a time (a row, a header), which the window turns into one card read per 512 bytes.
  class Window final : public formats::med::RandomSource {
   public:
    static constexpr std::uint32_t kBytes = 512;
    void attach(hal::storage::Storage& storage, hal::storage::FileHandle file, std::uint32_t size) noexcept;
    void detach() noexcept;
    std::uint32_t size() const noexcept override { return size_; }
    bool read_at(std::uint32_t offset, void* dst, std::uint32_t n) noexcept override;
    hal::storage::Status error() const noexcept { return error_; }
    std::uint32_t fills() const noexcept { return fills_; }

   private:
    bool fill(std::uint32_t base) noexcept;
    hal::storage::Storage* storage_ = nullptr;
    hal::storage::FileHandle file_{};
    std::uint32_t size_ = 0;
    std::uint32_t base_ = 0;
    std::uint32_t len_ = 0;  // bytes valid at base_ (0: no window)
    hal::storage::Status error_ = hal::storage::Status::Ok;
    std::uint32_t fills_ = 0;  // card reads so far (the step's budget)
    std::uint8_t buf_[kBytes] = {};
  };

  void fail(const char* text) noexcept;
  void summarise() noexcept;
  void name_from(const char* path) noexcept;

  hal::storage::Storage* storage_ = nullptr;
  hal::storage::FileHandle file_{};
  Window window_;
  formats::mod::ImportReader mod_;
  formats::med::ImportReader med_;
  formats::FormatReader* reader_ = nullptr;
  bool is_med_ = false;
  bool touched_ = false;  // reader_->begin() ran (the first step): it resets the project
  model::ModelArena* arena_ = nullptr;
  State state_ = State::Idle;
  bool active_ = false;
  ModuleSummary summary_;
};

}  // namespace o3os::app
