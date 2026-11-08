// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Imports a module file of the card (OS-FILES part A, docs/specs/OS-FILES-IMPORT.md; new Community OS behaviour) into the document: a ProTracker .mod or Scream Tracker 3 .s3m
// (formats/mod/import.h, OS-MODIMP) or an OctaMED .med / .mmd0..3 (formats/med/import.h, OS-008b). The importers run in the importer module of the card
// (modules/importers/tracker.tmod, OS-FLASHDIET I3: the image does not link them); this is the proxy around that module, the counterpart of
// NativeFolderLoader and OgSampleLoader for the File screen's IMPORT page. The platform runs it as the phases of a project load (the settle before, the sample PCM and
// the SD modules after):
//
//   Project phase   ModuleImport::begin + step: the file is read through a random-access window over the card (an MMD module is a graph of offsets, and the MOD / S3M decoders go back and
//                   forth), the reader is stepped a bounded number of cells and card reads at a time, and the project is made. ORDER: begin() only checks the path, opens the file and records
//                   what the first step needs (the platform calls it BEFORE the settle hand-over, while the render still reads the document); the importer's first act, which resets the
//                   project, is the first step(), which runs only after Settle (the render has let the voices die and the transport is stopped), as the project loaders' writes do. A file that
//                   will not open (not found, a card error), or an importer file of the card that is missing or for another firmware, fails at begin() with Start::Failed and the document is
//                   as it was (document_touched() false); the platform then does NOT hand over to begin_replace at all (import_replaces_document), so the open project's SD modules and
//                   effect inserts stay loaded too, and the strip shows the refusal at once. Any failure after that first step, a bad signature or a cut-off header included, leaves the document
//                   EMPTY (reset, valid): the platform then also empties the voices' sample table and the bank, which held the old PCM.
//   Samples phase   OgSampleLoader::begin_feed(import, ...): the importers do not write the PCM, they decode it on request (Importer::read_pcm: 8-bit times 256, 16-bit as it is, a stereo S3M
//                   sample its left channel), and ModuleImport is the PcmFeed that asks them. The samples go straight into the sample arena, bound to the voices at the module's own rate; they are
//                   on no card until the user saves (the bank does not mark them).
//   then            close(): the file is released, the sample counts go into the summary.
//
// The summary (ModuleSummary) is the import's report for the user: what was made, what was not kept, by name (the importers' drop groups), how many commands were approximated or dropped, and the
// reason when it failed. The File screen's REPORT page draws it (module_report_text).
//
// OS-FLASHDIET I2/I3: with a module host handed over (set_module_host, by the platform's composition root), the import runs through the
// importer module of the card (modules/importers/<stem>.tmod, kind 3, ABI 1.11): begin() also checks that file's 64-byte header (a missing
// file or one for another firmware fails there, with the document untouched), the first step() after Settle loads the importer into the
// module region (the old project's SD modules were unloaded at the Settle) and begins the import in it, read_pcm is the module's, and
// close() unloads it. OS-FLASHDIET I3: the linked importers are gone — the image does not link formats/mod or formats/med any more
// (formats_tracker is the tests' and the host registry's library, not the image's), so the module host is the only path: a begin() without
// one is refused like a missing importer file. The tests' reference side of the equality sweep drives the readers of formats_tracker
// directly.
//
// Contexts: main loop only. READ ONLY on the card (open, read, seek, size, close). No heap: the proxy is a small object, a static of the
// platform in SDRAM.

#include <o3os/app/file_import.h>
#include <o3os/app/og_samples.h>
#include <o3os/formats/import_module.h>
#include <o3os/formats/med/import.h>
#include <o3os/formats/mod/import.h>
#include <o3os/hal/storage.h>
#include <o3os/model/project.h>
#include <o3os/module/loader.h>

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
  // OS-FLASHDIET I2 (QA-FIL-139/140): Failed is a begin() that refused the file before anything was touched (the importer file of the card missing or for another
  // firmware, or the module file itself will not open): the state is Failed at once and the document is as it was. The platforms must not hand such a begin over to the
  // replace (see import_replaces_document below).
  enum class Start : std::uint8_t { Started, BadPath, NotModule, Failed };
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
  // module reader takes the file (nothing changed, the last import's report stays). Failed: the file will not open, no module host was handed over, or the importer file of the card is missing or for another
  // firmware: the state is Failed at once and the document is untouched (document_touched() false). Nothing is written to `arena` here in any case: the importer starts at the first step().
  // `storage` and `arena` must outlive the import and the sample phase.
  // OS-FLASHDIET I2/I3: the import runs through the importer module of the card (modules/importers/<stem>.tmod): begin() also checks that file's 64-byte header
  // here (present, kind 3, ABI within the OS's, layout tag equal); a refusal is Start::Failed with NO IMPORTER (no file) or IMPORTER FOR OTHER FIRMWARE (a wrong one).
  Start begin(hal::storage::Storage& storage, const char* path, model::ModelArena& arena, ImportEngine engine) noexcept;
  // A slice of the work: about `byte_budget` / 8 cells (at least kMinCells) of the reader, a card read (a window fill) counting kFillCells. The first call resets the document.
  // Returns the state after it.
  State step(std::size_t byte_budget) noexcept;
  // The sample phase ended: its counts go into the summary, and the file is released.
  void samples_done(const SampleLoadReport& report) noexcept;
  // Releases the file (the PCM can no longer be read) and unloads the importer module if one was loaded. The summary stays.
  void close() noexcept;

  // OS-FLASHDIET I2/I3: the module host the platform hands over once, at its composition root: the set the importer is loaded into, the loader that loads it (stepped from step()), the code-sync
  // hook the loader needs after writing module code, and the importer's stem (its file is modules/importers/<stem>.tmod on the card; the set may already hold it as a native entry, as the host tests
  // and the simulator register it). The import always runs through it (I3: the linked readers are gone from the image).
  void set_module_host(module::ModuleSet* set, module::ModuleLoader* loader, module::ModuleLoader::SyncFn sync, const char* importer_stem) noexcept;

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
  void fail_importer_file(const char* text) noexcept;  // OS-FLASHDIET I2: a refusal of the importer file, with the report page's two lines of 3.6
  void summarise() noexcept;
  void name_from(const char* path) noexcept;

  // OS-FLASHDIET I2: the importer module path (the proxy). check_importer_header() is begin()'s header-only check of modules/importers/<stem>.tmod; start_importer() is the first step's load
  // (or find of a native entry) plus init and the module's begin; unload_importer() is close()'s unload. format_index() maps the probed file onto the module's format list.
  State step_module(std::size_t cells) noexcept;
  module::Status check_importer_header() noexcept;
  bool start_importer() noexcept;
  bool begin_import() noexcept;
  void unload_importer() noexcept;
  std::uint32_t format_index() const noexcept;
  void copy_report() noexcept;
  void build_host() noexcept;
  static std::int32_t window_read(void* ctx, std::uint32_t offset, void* dst, std::uint32_t n) noexcept;
  static std::uint32_t window_size(void* ctx) noexcept;
  static void host_log(const char*) noexcept {}
  static float host_exp2(float x) noexcept;
  static float host_log2(float x) noexcept;
  static float host_sin(float x) noexcept;
  static float host_cos(float x) noexcept;

  enum class ImpPhase : std::uint8_t { None, Loading, Running };

  hal::storage::Storage* storage_ = nullptr;
  hal::storage::FileHandle file_{};
  Window window_;
  bool touched_ = false;  // the first step ran: it reset the document
  model::ModelArena* arena_ = nullptr;
  State state_ = State::Idle;
  bool active_ = false;
  ModuleSummary summary_;

  // OS-FLASHDIET I2: the module host and the importer module it loads.
  module::ModuleSet* modules_ = nullptr;
  module::ModuleLoader* loader_ = nullptr;
  module::ModuleLoader::SyncFn sync_ = nullptr;
  char importer_stem_[module::kStemMax + 1] = {};
  const module::LoadedModule* imp_ = nullptr;
  void* imp_state_ = nullptr;
  module::ModuleSet::StateMark arena_mark_{};
  ImpPhase imp_phase_ = ImpPhase::None;
  bool imp_from_card_ = false;  // loaded from the card by us (unload gives the arenas back); false: the set already held it (a native entry)
  Probe probe_ = Probe::NotModule;
  ImportEngine engine_ = ImportEngine::Auto;
  o3m_host host_{};
  o3m_file_window file_window_{};
  formats::ImportModuleReport report_{};
  char detail_buf_[96] = {};  // summary_.detail points here for the importer-file refusals (two lines, '\n'-separated)
};

// OS-FLASHDIET I2 (QA-FIL-139/140): the one decision the platforms take on begin()'s result, kept here so a host test reaches it: only Started hands over to the replace
// (the platform's begin_replace: the Settle then unloads the open project's modules and inserts, and the first step resets the document). Failed does NOT: the open project,
// its SD instruments and effect inserts stay exactly as they are, and the strip shows the refusal at once (the import is not busy, so the app's next poll reads
// import_summary()). BadPath and NotModule start nothing either, but have no report of their own to show.
inline bool import_replaces_document(ModuleImport::Start started) noexcept { return started == ModuleImport::Start::Started; }

}  // namespace o3os::app
