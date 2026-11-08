// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/module_import.h>

#include <o3os/app/dir_scan.h>
#include <o3os/app/import_job.h>
#include <o3os/core/fast_math.h>
#include <o3os/module/image.h>

#include "text_line.h"

#include <cstddef>
#include <cstring>

namespace o3os::app {

namespace {

using hal::storage::Status;
namespace med = formats::med;
namespace mod = formats::mod;

constexpr std::size_t kCellsPerByte = 8;  // the platform's byte budget of a pass, as cells of the importer's work

constexpr char upper(char c) noexcept { return c >= 'a' && c <= 'z' ? static_cast<char>(c - ('a' - 'A')) : c; }

// Copies `s` into the fixed array `out` (cap characters + terminator), upper case, '?' for anything that is not printable ASCII; longer text ends in '~'.
void put_upper(char* out, std::size_t cap, const char* s) noexcept {
  std::size_t n = 0;
  bool cut = false;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p != '\0'; ++p) {
    if (n == cap) {
      cut = true;
      break;
    }
    out[n++] = (*p < 0x20u || *p >= 0x7Fu) ? '?' : upper(static_cast<char>(*p));
  }
  if (cut && cap != 0u) {
    out[cap - 1u] = '~';
  }
  out[n] = '\0';
}

}  // namespace

// ---- The summary ---------------------------------------------------------------------------------------------------------------------------
const char* ModuleSummary::group_name(unsigned bit) const noexcept { return format == Format::Med ? med::drop_group_name(bit) : mod::drop_group_name(bit); }

void module_report_text(const ModuleSummary& s, ModuleReportText& out) noexcept {
  out.count = 0;
  const auto line = [&out]() -> Line {
    char* const dst = out.line[out.count < ModuleReportText::kLines ? out.count : ModuleReportText::kLines - 1u];
    if (out.count < ModuleReportText::kLines) {
      ++out.count;
    }
    return Line(dst, ModuleReportText::kChars + 1u);
  };
  if (!s.valid) {
    (void)line().str("NO IMPORT YET");
    return;
  }
  (void)line().str(s.name);
  if (!s.ok) {
    (void)line().str("FAILED    ").str(s.error);
    // OS-FLASHDIET I2: the importer file's refusals carry two hint lines of their own (docs/specs/OS-FLASHDIET.md 3.6), '\n'-separated in `detail`
    for (const char* p = s.detail; *p != '\0';) {
      Line l = line();
      l.str("          ");
      for (; *p != '\0' && *p != '\n'; ++p) {
        const char c[2] = {upper(*p), '\0'};
        l.str(c);
      }
      if (*p == '\n') {
        ++p;
      }
    }
    return;
  }
  {
    Line l = line();
    l.str("FORMAT    ").str(s.format == ModuleSummary::Format::Med ? "OCTAMED" : (s.format == ModuleSummary::Format::S3m ? "S3M" : "MOD")).str(", ").dec(s.channels);
    l.str(s.format == ModuleSummary::Format::Med ? " TRACKS" : " CHANNELS");
  }
  (void)line().str("COMMANDS  ").str(s.amiga_commands ? "AMIGA ENGINE (FX 128+)" : "MODERN ENGINE (CORE FX)");
  {
    Line l = line();
    l.str("SONG      ").dec(s.patterns).str(" PATTERNS, ").dec(s.order).str(" ORDER, ").dec(s.tracks_kept).str(" TRACKS");
  }
  if (s.tracks_dropped != 0u) {  // (a line of its own: the numbers above can fill the one before)
    (void)line().str("          ").dec(s.tracks_dropped).str(" TRACKS CUT");
  }
  {
    Line l = line();
    l.str("SAMPLES   ").dec(s.instruments).str(" MADE, ").dec(s.samples_loaded).str(" LOADED, ").dec((s.samples_kb)).str(" KB");
  }
  if (s.samples_no_room != 0u || s.samples_silent != 0u || s.instruments_refused != 0u) {
    Line l = line();
    l.str("          ");
    bool any = false;
    const auto part = [&l, &any](unsigned n, const char* what) {
      if (n != 0u) {
        l.str(any ? ", " : "").dec(n).str(what);
        any = true;
      }
    };
    part(s.samples_no_room, " NO ROOM");
    part(s.samples_silent, " SILENT");
    part(s.instruments_refused, " REFUSED");
  }
  (void)line().str("EFFECTS   ").dec(s.cmd_approx).str(" APPROXIMATED, ").dec(s.cmd_dropped).str(" DROPPED");
  if (s.clamped != 0u) {
    (void)line().str("CLAMPED   ").dec(s.clamped).str(" VALUES");
  }
  if (s.dropped_groups != 0u) {
    (void)line().str("NOT KEPT OR APPROXIMATED:");
    for (unsigned bit = 0; bit < 32u && out.count < ModuleReportText::kLines; ++bit) {
      if (((s.dropped_groups >> bit) & 1u) != 0u) {
        char* const dst = out.line[out.count++];
        Line l(dst, ModuleReportText::kChars + 1u);
        l.str("  ");
        for (const char* p = s.group_name(bit); *p != '\0'; ++p) {
          const char c[2] = {upper(*p), '\0'};
          l.str(c);
        }
      }
    }
  }
}

void module_strip_text(const ModuleSummary& s, char* out, std::size_t cap) noexcept {
  if (cap == 0u) {
    return;
  }
  const char* prefix = s.ok ? "IMPORTED " : "IMPORT ";
  char suffix[64];
  {
    Line l(suffix, sizeof suffix);
    if (!s.ok) {
      l.str(" FAILED: ").str(s.error);
    } else if (s.samples_no_room != 0u || s.dropped_groups != 0u) {
      l.str(": ");
      if (s.samples_no_room != 0u) {
        l.dec(s.samples_no_room).str(" NO ROOM, ");
      }
      l.str("F8 REPORT");
    }
  }
  Line line(out, cap);
  line.str(prefix);
  const std::size_t used = line.size() + std::strlen(suffix);
  const std::size_t room = cap > used + 1u ? cap - 1u - used : 0u;
  char name[ModuleSummary::kNameChars + 1];
  std::size_t n = 0;
  for (; s.name[n] != '\0' && n < room && n < ModuleSummary::kNameChars; ++n) {
    name[n] = s.name[n];
  }
  name[n] = '\0';
  line.str(name).str(suffix);
}

// ---- The window over the file --------------------------------------------------------------------------------------------------------------
void ModuleImport::Window::attach(hal::storage::Storage& storage, hal::storage::FileHandle file, std::uint32_t size) noexcept {
  storage_ = &storage;
  file_ = file;
  size_ = size;
  base_ = 0;
  len_ = 0;
  error_ = Status::Ok;
  fills_ = 0;
}

void ModuleImport::Window::detach() noexcept {
  storage_ = nullptr;
  file_ = hal::storage::FileHandle{};
  len_ = 0;
}

bool ModuleImport::Window::fill(std::uint32_t base) noexcept {
  len_ = 0;
  if (storage_ == nullptr) {
    return false;
  }
  ++fills_;
  Status s = storage_->seek(file_, base);
  std::size_t have = 0;
  while (s == Status::Ok && have < kBytes) {
    std::size_t got = 0;
    s = storage_->read(file_, buf_ + have, kBytes - have, got);
    if (s != Status::Ok || got == 0u || got > kBytes - have) {
      s = s != Status::Ok ? s : (got == 0u ? Status::Ok : Status::Io);
      break;
    }
    have += got;
  }
  if (s != Status::Ok) {
    error_ = s;
    return false;
  }
  base_ = base;
  len_ = static_cast<std::uint32_t>(have);
  return true;
}

bool ModuleImport::Window::read_at(std::uint32_t offset, void* dst, std::uint32_t n) noexcept {
  if (n == 0u) {
    return true;
  }
  if (offset > size_ || n > size_ - offset) {
    return false;
  }
  std::uint8_t* out = static_cast<std::uint8_t*>(dst);
  while (n != 0u) {
    const std::uint32_t base = offset & ~(kBytes - 1u);
    if (len_ == 0u || base_ != base) {
      if (!fill(base)) {
        return false;
      }
    }
    const std::uint32_t at = offset - base;
    if (at >= len_) {
      return false;  // the file is shorter than its size said
    }
    std::uint32_t take = len_ - at;
    take = take > n ? n : take;
    std::memcpy(out, buf_ + at, take);
    out += take;
    offset += take;
    n -= take;
  }
  return true;
}

// ---- The import ----------------------------------------------------------------------------------------------------------------------------
void ModuleImport::name_from(const char* path) noexcept {
  const char* name = path;
  for (const char* p = path; *p != '\0'; ++p) {
    name = *p == '/' ? p + 1 : name;
  }
  put_upper(summary_.name, ModuleSummary::kNameChars, name);
}

void ModuleImport::fail(const char* text) noexcept {
  summary_.ok = false;
  put_upper(summary_.error, sizeof summary_.error - 1u, text);
  close();
  state_ = State::Failed;
}

// OS-FLASHDIET I2 (3.6): a refusal of the importer file (no modules/importers/<stem>.tmod, or one for another firmware) fails at begin() with the
// report page's two extra lines: NEEDS MODULES/IMPORTERS/TRACKER.TMOD and FROM THE STARTER ZIP OF THIS RELEASE (each within the page's 46 characters).
void ModuleImport::fail_importer_file(const char* text) noexcept {
  std::size_t n = 0;
  const char* const parts[2] = {"needs modules/importers/", ".tmod\nfrom the starter zip of this release"};
  // "needs modules/importers/<stem>.tmod\nfrom the starter zip of this release"
  for (const char* p = parts[0]; *p != '\0' && n + 1u < sizeof detail_buf_; ++p) {
    detail_buf_[n++] = *p;
  }
  for (const char* p = importer_stem_; *p != '\0' && n + 1u < sizeof detail_buf_; ++p) {
    detail_buf_[n++] = *p;
  }
  for (const char* p = parts[1]; *p != '\0' && n + 1u < sizeof detail_buf_; ++p) {
    detail_buf_[n++] = *p;
  }
  detail_buf_[n] = '\0';
  summary_.detail = detail_buf_;
  fail(text);
}

ModuleImport::Probe ModuleImport::decide(const char* name, const std::uint8_t* head, std::size_t n, std::uint32_t size) noexcept {
  // What the bytes are: the registry's own tests (formats/src/builtin_formats.cpp sniff_med, sniff_mod, sniff_s3m; the S3M and MOD ones are formats::mod::sniff_kind).
  Probe seen = Probe::NotModule;
  if (n >= 4u && std::memcmp(head, "MMD", 3) == 0 && head[3] >= '0' && head[3] <= '3' && size >= med::caps::kHeaderBytes + med::caps::kSongBytes) {
    seen = Probe::Med;
  } else {
    const mod::Kind k = mod::sniff_kind(head, n);
    seen = k == mod::Kind::Mod ? Probe::Mod : (k == mod::Kind::S3m ? Probe::S3m : Probe::NotModule);
    if (seen == Probe::Mod && size < mod::tagged_mod_min_bytes(head, n)) {
      seen = Probe::NotModule;  // OS-BUG-IMPTRUNC: no room for one pattern: the same refusal as a cut file, before the NEW question
    }
  }
  const char* base = name;
  for (const char* p = name; *p != '\0'; ++p) {
    base = *p == '/' ? p + 1 : base;
  }
  const ModuleName said = module_name_kind(base);
  // OS-IMPORT-ANY I6: a 15-sample Soundtracker file has no signature, so it is tried only when the name says MOD or says nothing (the sniff never claims it).
  if (seen == Probe::NotModule && (said == ModuleName::None || said == ModuleName::Mod) && mod::is_soundtracker15(head, n, size)) {
    seen = Probe::Mod;
  }
  if (seen == Probe::NotModule) {
    return seen;
  }
  if (said != ModuleName::None && (said == ModuleName::Med) != (seen == Probe::Med)) {
    return Probe::NotModule;  // the name says OctaMED and the bytes a MOD / S3M, or the other way round (a .mod that holds an S3M is one reader's: it goes on)
  }
  return seen;
}

ModuleImport::Probe ModuleImport::probe(hal::storage::Storage& storage, const char* path) noexcept {
  if (path == nullptr || !hal::storage::valid_path(path)) {
    return Probe::NotModule;
  }
  hal::storage::FileHandle file{};
  std::uint32_t size = 0;
  if (storage.open(path, file) != Status::Ok) {
    return Probe::Unreadable;
  }
  std::uint8_t head[formats::kSniffBytes];
  std::size_t have = 0;
  Status s = storage.size(file, size);
  const std::size_t want = size < sizeof head ? size : sizeof head;
  while (s == Status::Ok && have < want) {
    std::size_t got = 0;
    s = storage.read(file, head + have, want - have, got);
    if (s == Status::Ok && (got == 0u || got > want - have)) {
      s = Status::Io;
    }
    have += s == Status::Ok ? got : 0u;
  }
  storage.close(file);
  return s == Status::Ok ? decide(path, head, have, size) : Probe::Unreadable;
}

ModuleImport::Start ModuleImport::begin(hal::storage::Storage& storage, const char* path, model::ModelArena& arena, ImportEngine engine) noexcept {
  // OS-FLASHDIET I3 review: with no module host there is nothing to load or run the importer with: refuse at the top, whatever the card holds
  // (a valid modules/importers/<stem>.tmod included — its header check below could pass) — the first step would dereference the null host.
  if (modules_ == nullptr || loader_ == nullptr) {
    fail("NO IMPORTER");
    return Start::Failed;
  }
  if (path == nullptr || !hal::storage::valid_path(path)) {
    return Start::BadPath;  // (nothing changes: the last import's report stays)
  }
  // OS-IMPORT-ANY I3: the reader is the bytes' (the name only has to agree). A file the card will not give (Unreadable) goes on: the open below fails with its own words, as it always has.
  const Probe seen = probe(storage, path);
  if (seen == Probe::NotModule) {
    return Start::NotModule;
  }
  close();
  state_ = State::Idle;
  summary_ = ModuleSummary{};
  active_ = true;
  touched_ = false;
  arena_ = &arena;
  storage_ = &storage;
  summary_.valid = true;
  name_from(path);
  const Status opened = storage.open(path, file_);
  std::uint32_t size = 0;
  if (opened != Status::Ok || storage.size(file_, size) != Status::Ok) {
    fail(opened == Status::NotFound ? "FILE NOT FOUND" : "READ ERROR");
    return Start::Failed;  // OS-FLASHDIET I2 (QA-FIL-139/140): refused at begin: the platform does not hand it to the replace, the document stays as it was
  }
  window_.attach(storage, file_, size);
  probe_ = seen;
  engine_ = engine;
  // OS-FLASHDIET I2/I3: the importer file's header is checked here, before anything is placed or reset: a missing file or one for another
  // firmware (wrong kind, ABI or layout tag) fails at begin() and the document stays as it was (no module host at all is refused at the top).
  // A set that already holds the importer (a native entry: the host tests and the simulator register it) is this build's own, version-locked
  // by construction, so the card file is not consulted.
  if (modules_ == nullptr || modules_->find_importer(importer_stem_) < 0) {
    const module::Status imp = check_importer_header();
    if (imp != module::Status::Ok) {
      fail_importer_file(imp == module::Status::NotFound || imp == module::Status::Io || imp == module::Status::TooShort ? "NO IMPORTER" : "IMPORTER FOR OTHER FIRMWARE");
      return Start::Failed;  // QA-FIL-139/140: the platform skips begin_replace and shows the strip directly, with the open project and its modules untouched
    }
  }
  state_ = State::Running;
  return Start::Started;
}

ModuleImport::State ModuleImport::step(std::size_t byte_budget) noexcept {
  if (state_ != State::Running) {
    return state_;
  }
  std::size_t cells = byte_budget / kCellsPerByte;
  cells = cells < kMinCells ? kMinCells : cells;
  return step_module(cells);
}

// OS-FLASHDIET I2/I3: the module path of step() — the only path now (I3: the linked readers are gone from the image). The first call resets
// the document (as the linked reader's begin did), then loads the importer into the module region (the old project's SD modules were
// unloaded at the platform's Settle, so the region is as free as it gets) and begins the import in it; later calls run the module's step()
// until the budget is spent. A load that fails after the reset leaves the document empty (3.6): NO MODULE MEMORY or IMPORTER DAMAGED.
ModuleImport::State ModuleImport::step_module(std::size_t cells) noexcept {
  std::size_t spent = 0;
  if (!touched_) {
    touched_ = true;
    arena_->project.reset();  // the first step resets the document (the platform has settled by now)
    if (!start_importer()) {
      return state_;  // fail() has said why (NO MODULE MEMORY / IMPORTER DAMAGED)
    }
  }
  if (imp_phase_ == ImpPhase::Loading) {  // the loader is stepped across passes, within this step's budget
    while (loader_->state() == module::ModuleLoader::State::Running && spent < cells) {
      (void)loader_->step(kMinCells);
      spent += kMinCells;  // bytes of the importer's file count as budget
    }
    if (loader_->state() == module::ModuleLoader::State::Running) {
      return state_;
    }
    if (loader_->state() != module::ModuleLoader::State::Done) {
      fail(loader_->status() == module::Status::NoMemory ? "NO MODULE MEMORY" : "IMPORTER DAMAGED");
      return state_;
    }
    imp_ = &loader_->module();
    imp_from_card_ = true;
    imp_phase_ = ImpPhase::Running;
    if (!begin_import()) {
      return state_;
    }
  }
  const o3m_module* entry = imp_ != nullptr ? imp_->entry : nullptr;
  const o3m_importer* t = entry != nullptr ? entry->importer : nullptr;
  if (t == nullptr || t->step == nullptr) {
    fail("IMPORTER DAMAGED");
    return state_;
  }
  while (spent < cells) {
    const std::uint32_t fills = window_.fills();
    const std::uint32_t r = t->step(imp_state_, kMinCells);
    spent += kMinCells + static_cast<std::size_t>(window_.fills() - fills) * kFillCells;
    if (r == O3M_IMPORT_RUNNING) {
      continue;
    }
    copy_report();
    if (r == O3M_IMPORT_DONE) {
      summarise();
      state_ = State::Done;
      return state_;
    }
    const formats::FormatReport& rep = report_.common;
    summary_.detail = rep.detail != nullptr ? rep.detail : "";
    fail(report_.overflow ? "SONG NEEDS MORE PATTERNS THAN A PROJECT HOLDS"
                          : (window_.error() != Status::Ok ? "READ ERROR" : import_failure_text(rep)));
    return state_;
  }
  return state_;
}

// begin()'s header-only check of modules/importers/<stem>.tmod: the file is opened, its 64-byte header read and checked (check_header), and it is closed again;
// the loader opens it again at the first step. Ok, or the Status that says what is wrong (NotFound / Io / TooShort: no importer; anything else: one for another firmware).
module::Status ModuleImport::check_importer_header() noexcept {
  char path[24 + module::kStemMax + 8];
  std::size_t n = 0;
  for (const char* p = "modules/importers/"; *p != '\0'; ++p) {
    path[n++] = *p;
  }
  for (const char* p = importer_stem_; *p != '\0'; ++p) {
    path[n++] = *p;
  }
  for (const char* p = ".tmod"; *p != '\0'; ++p) {
    path[n++] = *p;
  }
  path[n] = '\0';
  hal::storage::FileHandle f{};
  if (storage_->open(path, f) != Status::Ok) {
    return module::Status::NotFound;
  }
  std::uint32_t size = 0;
  if (storage_->size(f, size) != Status::Ok) {
    storage_->close(f);
    return module::Status::Io;
  }
  o3m_file_header h{};
  std::size_t have = 0;
  while (have < sizeof h) {
    std::size_t got = 0;
    const Status s = storage_->read(f, reinterpret_cast<std::uint8_t*>(&h) + have, sizeof h - have, got);
    if (s != Status::Ok || got == 0u || got > sizeof h - have) {
      storage_->close(f);
      return module::Status::Io;  // a short or failed read is an I/O refusal, whatever the status said
    }
    have += got;
  }
  storage_->close(f);
  return module::check_header(h, size);
}

// The first step's load: the set may already hold the importer (a native entry: the host tests and the simulator register it), else the loader starts
// loading modules/importers/<stem>.tmod into the region (ImpPhase::Loading; step() drives it on). With the module there, its state block is taken,
// init() called and the import begun in it.
bool ModuleImport::start_importer() noexcept {
  arena_mark_ = modules_->state_mark();
  const int at = modules_->find_importer(importer_stem_);
  if (at >= 0) {
    imp_ = modules_->importer_at(static_cast<unsigned>(at));
    imp_from_card_ = false;
    imp_phase_ = ImpPhase::Running;
    return begin_import();
  }
  if (!loader_->begin(*storage_, *modules_, importer_stem_, sync_, true)) {  // true: the importer scan (modules/importers/ only)
    fail(loader_->status() == module::Status::NoMemory ? "NO MODULE MEMORY" : "IMPORTER DAMAGED");
    return false;
  }
  imp_phase_ = ImpPhase::Loading;
  return true;
}

bool ModuleImport::begin_import() noexcept {
  const o3m_module* entry = imp_ != nullptr ? imp_->entry : nullptr;
  if (entry == nullptr || entry->importer == nullptr || entry->importer->begin == nullptr) {
    fail("IMPORTER DAMAGED");
    return false;
  }
  std::uint8_t* mem = nullptr;
  std::uint32_t addr = 0;
  if (!modules_->take_state(imp_->header.state_bytes, mem, addr)) {
    fail("NO MODULE MEMORY");
    return false;
  }
  imp_state_ = mem;
  build_host();
  if (entry->init(imp_state_, &host_) != 0) {
    fail("IMPORTER DAMAGED");
    return false;
  }
  formats::ImportModuleOptions opts{};
  opts.engine = static_cast<std::uint8_t>(engine_);
  file_window_ = o3m_file_window{sizeof(o3m_file_window), &window_, &ModuleImport::window_read, &ModuleImport::window_size};  // struct_size first
  if (entry->importer->begin(imp_state_, &file_window_, format_index(), arena_, &opts, sizeof opts) != 0) {
    fail("IMPORTER DAMAGED");
    return false;
  }
  return true;
}

// close()'s unload: the module's close() and shutdown(), the state block and (when we loaded it from the card) the module itself back to the set, and the
// arenas to the marks taken before the load.
void ModuleImport::unload_importer() noexcept {
  if (imp_ != nullptr) {
    const o3m_module* entry = imp_->entry;
    if (entry != nullptr && imp_state_ != nullptr) {
      if (entry->importer != nullptr && entry->importer->close != nullptr) {
        entry->importer->close(imp_state_);
      }
      if (entry->shutdown != nullptr) {
        entry->shutdown(imp_state_);
      }
    }
    if (imp_from_card_) {
      (void)modules_->drop_importer(importer_stem_);
    }
    modules_->state_rollback(arena_mark_);
  }
  imp_ = nullptr;
  imp_state_ = nullptr;
  imp_from_card_ = false;
  imp_phase_ = ImpPhase::None;
}

// The module's format list entry for the probed file: "mod", "s3m" or "med" (the module's table order is its own; the OS offers the file by extension).
std::uint32_t ModuleImport::format_index() const noexcept {
  const o3m_module* entry = imp_ != nullptr ? imp_->entry : nullptr;
  const o3m_importer* t = entry != nullptr ? entry->importer : nullptr;
  if (t == nullptr) {
    return 0;
  }
  const char* const want = probe_ == Probe::Med ? "med" : (probe_ == Probe::S3m ? "s3m" : "mod");
  for (std::uint32_t i = 0; i < t->format_count && i < O3M_IMPORTER_FORMATS_MAX; ++i) {
    if (t->formats != nullptr && t->formats[i].ext != nullptr && std::strcmp(t->formats[i].ext, want) == 0) {
      return i;
    }
  }
  return 0;
}

void ModuleImport::copy_report() noexcept {
  const o3m_module* entry = imp_ != nullptr ? imp_->entry : nullptr;
  const o3m_importer* t = entry != nullptr ? entry->importer : nullptr;
  if (t != nullptr && t->report != nullptr) {
    (void)t->report(imp_state_, &report_, static_cast<std::uint32_t>(sizeof report_));
  }
}

// The o3m_host the importer's init() is given: the OS's libm-free exp2 / log2; sin / cos are stubs (the importers need none of them). Its `size` stays the 52 bytes of ABI 1.0..1.11
// (OS-INSFX-ABI2 A-12): the 1.12 members (get_clock, track_level) are not lent to an importer, so O3M_HOST_HAS is false for them there and no importer sees a NULL function.
void ModuleImport::build_host() noexcept {
  host_ = o3m_host{};
  host_.abi_major = O3M_ABI_MAJOR;
  host_.abi_minor = O3M_ABI_MINOR;
  host_.size = static_cast<std::uint32_t>(offsetof(o3m_host, get_clock));
  host_.sample_rate = 44100.0f;
  host_.log = &ModuleImport::host_log;
  host_.exp2_f = &ModuleImport::host_exp2;
  host_.log2_f = &ModuleImport::host_log2;
  host_.sin_f = &ModuleImport::host_sin;
  host_.cos_f = &ModuleImport::host_cos;
}

float ModuleImport::host_exp2(float x) noexcept { return core::exp2_fast(x); }
float ModuleImport::host_log2(float x) noexcept { return core::log2_fast(x); }

// The importers need no sin / cos (the host table is complete anyway): a stub, not a copy of audio::ModuleVoices' polynomial (the copy was dead .text).
float ModuleImport::host_sin(float) noexcept { return 0.0f; }
float ModuleImport::host_cos(float) noexcept { return 0.0f; }

std::int32_t ModuleImport::window_read(void* ctx, std::uint32_t offset, void* dst, std::uint32_t n) noexcept {
  return static_cast<Window*>(ctx)->read_at(offset, dst, n) ? 0 : -1;
}
std::uint32_t ModuleImport::window_size(void* ctx) noexcept { return static_cast<Window*>(ctx)->size(); }

// OS-FLASHDIET I2/I3: the report the module copied out (I3: the only side — the linked readers are gone).
void ModuleImport::summarise() noexcept {
  const formats::FormatReport& rep = report_.common;
  summary_.ok = true;
  summary_.dropped_groups = rep.dropped_groups;
  summary_.clamped = rep.sanitised;
  if (report_.is_med) {
    summary_.format = ModuleSummary::Format::Med;
    summary_.amiga_commands = report_.engine_fx;
    summary_.channels = report_.channels;
    summary_.patterns = report_.patterns;
    summary_.order = report_.order;
    summary_.tracks_kept = report_.tracks_kept;
    summary_.tracks_dropped = report_.tracks_dropped;
    summary_.instruments = report_.instruments;
    summary_.instruments_refused = report_.instruments_refused;
    summary_.cmd_approx = report_.cmd_approx;
    summary_.cmd_dropped = report_.cmd_dropped;
    return;
  }
  summary_.format = report_.mod_kind == static_cast<std::uint8_t>(mod::Kind::S3m) ? ModuleSummary::Format::S3m : ModuleSummary::Format::Mod;
  summary_.amiga_commands = report_.engine_fx;
  summary_.channels = report_.channels;
  summary_.patterns = report_.patterns;
  summary_.order = report_.order;
  summary_.tracks_kept = report_.tracks_kept;
  summary_.tracks_dropped = report_.tracks_dropped;
  summary_.instruments = report_.instruments;
  summary_.instruments_refused = report_.instruments_refused;
  summary_.cmd_approx = report_.cmd_approx;
  summary_.cmd_dropped = report_.cmd_dropped;
}

void ModuleImport::samples_done(const SampleLoadReport& report) noexcept {
  const auto cap16 = [](std::uint32_t v) { return static_cast<std::uint16_t>(v > 0xFFFFu ? 0xFFFFu : v); };
  summary_.samples_loaded = cap16(report.loaded);
  summary_.samples_no_room = cap16(report.no_room);
  summary_.samples_silent = cap16(report.missing);
  summary_.samples_kb = (report.samples * 2u + 512u) / 1024u;
  close();
}

void ModuleImport::set_module_host(module::ModuleSet* set, module::ModuleLoader* loader, module::ModuleLoader::SyncFn sync, const char* importer_stem) noexcept {
  modules_ = set;
  loader_ = loader;
  sync_ = sync;
  std::size_t n = 0;
  for (const char* p = importer_stem; p != nullptr && *p != '\0' && n + 1u < sizeof importer_stem_; ++p) {
    importer_stem_[n++] = *p;
  }
  importer_stem_[n] = '\0';
}

void ModuleImport::close() noexcept {
  unload_importer();  // OS-FLASHDIET I2: the importer module goes back to the set (and its arenas) here
  window_.detach();
  if (storage_ != nullptr && file_.is_open()) {
    storage_->close(file_);
  }
  file_ = hal::storage::FileHandle{};
}

std::uint32_t ModuleImport::frames(unsigned slot) const noexcept {
  if (state_ != State::Done || imp_ == nullptr || slot > med::caps::kMaxSamples) {
    return 0;
  }
  return report_.slots[slot].result == static_cast<std::uint8_t>(med::InstResult::Sample) ? report_.slots[slot].frames : 0u;
}

std::uint32_t ModuleImport::rate(unsigned slot) const noexcept {
  if (state_ != State::Done || imp_ == nullptr || slot > med::caps::kMaxSamples) {
    return 0;
  }
  return report_.slots[slot].rate;
}

bool ModuleImport::read(unsigned slot, std::uint32_t first, std::uint32_t n, std::int16_t* dst) noexcept {
  if (state_ != State::Done) {
    return false;
  }
  const o3m_module* entry = imp_ != nullptr ? imp_->entry : nullptr;  // OS-FLASHDIET I2: the module's read_pcm (I3: the only side)
  const o3m_importer* t = entry != nullptr ? entry->importer : nullptr;
  if (t == nullptr || t->read_pcm == nullptr) {
    return false;
  }
  return t->read_pcm(imp_state_, slot, first, n, dst) == 0;
}

}  // namespace o3os::app
