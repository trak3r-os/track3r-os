// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/module_import.h>

#include <o3os/app/dir_scan.h>
#include <o3os/app/import_job.h>

#include "text_line.h"

#include <cstring>

namespace o3os::app {

namespace {

using hal::storage::Status;
namespace med = formats::med;
namespace mod = formats::mod;

constexpr std::size_t kCellsPerByte = 8;  // the platform's byte budget of a pass, as cells of the importers' work

// The importers never read a byte of the ByteSource of FormatReader::begin (they take ReadParams::random): this is the one they are given.
class NoBytes final : public core::ByteSource {
 public:
  core::IoResult read(void*, std::size_t) noexcept override { return {0, core::IoStatus::Failed}; }
};
NoBytes g_no_bytes;

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
    if (s.detail[0] != '\0') {
      Line l = line();
      l.str("          ");
      for (const char* p = s.detail; *p != '\0'; ++p) {
        const char c[2] = {upper(*p), '\0'};
        l.str(c);
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
  if (path == nullptr || !hal::storage::valid_path(path)) {
    return Start::BadPath;  // (nothing changes: the last import's report stays)
  }
  // OS-IMPORT-ANY I3: the reader is the bytes' (the name only has to agree). A file the card will not give (Unreadable) goes on: the open below fails with its own words, as it always has.
  const Probe seen = probe(storage, path);
  if (seen == Probe::NotModule) {
    return Start::NotModule;
  }
  const bool med_ext = seen == Probe::Med || (seen == Probe::Unreadable && module_name_kind(path) == ModuleName::Med);
  close();
  state_ = State::Idle;
  summary_ = ModuleSummary{};
  active_ = true;
  touched_ = false;
  reader_ = nullptr;
  arena_ = &arena;
  is_med_ = med_ext;
  storage_ = &storage;
  summary_.valid = true;
  name_from(path);
  const Status opened = storage.open(path, file_);
  std::uint32_t size = 0;
  if (opened != Status::Ok || storage.size(file_, size) != Status::Ok) {
    fail(opened == Status::NotFound ? "FILE NOT FOUND" : "READ ERROR");
    return Start::Started;
  }
  window_.attach(storage, file_, size);
  // The options only; the readers' begin() resets the project, so it waits for the first step() (after the platform's Settle).
  if (is_med_) {
    reader_ = &med_;
    med_.options() = med::ImportReader::amiga_default();
    med_.options().engine_fx = engine != ImportEngine::Modern;  // Auto and Amiga: the engine window (OS-008c's default); Modern: the OG / core mapping
  } else {
    reader_ = &mod_;
    mod_.options() = mod::ImportOptions{};
    mod_.options().engine_fx = engine == ImportEngine::Auto ? mod::EngineFx::Auto : (engine == ImportEngine::Amiga ? mod::EngineFx::On : mod::EngineFx::Off);
  }
  state_ = State::Running;
  return Start::Started;
}

ModuleImport::State ModuleImport::step(std::size_t byte_budget) noexcept {
  if (state_ != State::Running) {
    return state_;
  }
  if (!touched_) {  // the first step: the reader resets the project (the platform has settled by now)
    touched_ = true;
    formats::ReadParams params;
    params.size_hint = window_.size();
    params.name = summary_.name;
    params.random = &window_;
    reader_->begin(g_no_bytes, *arena_, params);
  }
  std::size_t cells = byte_budget / kCellsPerByte;
  cells = cells < kMinCells ? kMinCells : cells;
  // Slices of kMinCells until the budget is spent, a card read costing kFillCells (see kFillCells).
  std::size_t spent = 0;
  formats::Progress p;
  do {
    const std::uint32_t fills = window_.fills();
    p = reader_->step(kMinCells);
    spent += kMinCells + static_cast<std::size_t>(window_.fills() - fills) * kFillCells;
  } while (p == formats::Progress::Running && spent < cells);
  switch (p) {
    case formats::Progress::Running: return state_;
    case formats::Progress::Done: summarise(); state_ = State::Done; return state_;
    case formats::Progress::Failed: break;
  }
  const formats::FormatReport& rep = reader_->report();
  const bool overflow = is_med_ ? med_.importer().report().overflow : mod_.importer().report().overflow;
  summary_.detail = rep.detail != nullptr ? rep.detail : "";
  fail(overflow ? "SONG NEEDS MORE PATTERNS THAN A PROJECT HOLDS"
               : (window_.error() != Status::Ok ? "READ ERROR" : import_failure_text(rep)));
  return state_;
}

void ModuleImport::summarise() noexcept {
  const formats::FormatReport& rep = reader_->report();
  summary_.ok = true;
  summary_.dropped_groups = rep.dropped_groups;
  summary_.clamped = rep.sanitised;
  if (is_med_) {
    const med::ImportReport& r = med_.importer().report();
    summary_.format = ModuleSummary::Format::Med;
    summary_.amiga_commands = med_.options().engine_fx;
    summary_.channels = static_cast<std::uint8_t>(r.tracks_used);
    summary_.patterns = r.patterns;
    summary_.order = r.order;
    summary_.tracks_kept = r.tracks_kept;
    summary_.tracks_dropped = r.tracks_dropped;
    summary_.instruments = r.instruments;
    summary_.instruments_refused = r.instruments_refused;
    for (unsigned i = 0; i < 256u; ++i) {
      summary_.cmd_approx += r.cmd_approx[i];
      summary_.cmd_dropped += r.cmd_dropped[i];
    }
    return;
  }
  const mod::ImportReport& r = mod_.importer().report();
  summary_.format = r.kind == mod::Kind::S3m ? ModuleSummary::Format::S3m : ModuleSummary::Format::Mod;
  summary_.amiga_commands = r.engine_fx;
  summary_.channels = r.channels;
  summary_.patterns = r.med.patterns;
  summary_.order = r.med.order;
  summary_.tracks_kept = r.med.tracks_kept;
  summary_.tracks_dropped = r.med.tracks_dropped;
  summary_.instruments = r.med.instruments;
  summary_.instruments_refused = static_cast<std::uint16_t>(r.med.instruments_refused + (r.instruments_beyond > 0xFFFFu ? 0xFFFFu : r.instruments_beyond));
  for (const mod::FxTally& f : r.fx) {
    summary_.cmd_approx += f.approx;
    summary_.cmd_dropped += f.dropped;
  }
}

void ModuleImport::samples_done(const SampleLoadReport& report) noexcept {
  const auto cap16 = [](std::uint32_t v) { return static_cast<std::uint16_t>(v > 0xFFFFu ? 0xFFFFu : v); };
  summary_.samples_loaded = cap16(report.loaded);
  summary_.samples_no_room = cap16(report.no_room);
  summary_.samples_silent = cap16(report.missing);
  summary_.samples_kb = (report.samples * 2u + 512u) / 1024u;
  close();
}

void ModuleImport::close() noexcept {
  window_.detach();
  if (storage_ != nullptr && file_.is_open()) {
    storage_->close(file_);
  }
  file_ = hal::storage::FileHandle{};
}

std::uint32_t ModuleImport::frames(unsigned slot) const noexcept {
  if (state_ != State::Done || reader_ == nullptr) {
    return 0;
  }
  if (is_med_) {
    return slot < med::caps::kMaxSamples && med_.importer().report().instrument[slot].result == med::InstResult::Sample ? med_.importer().pcm_frames(slot) : 0u;
  }
  return slot < mod::caps::kSlots && mod_.importer().report().med.instrument[slot].result == med::InstResult::Sample ? mod_.importer().pcm_frames(slot) : 0u;
}

std::uint32_t ModuleImport::rate(unsigned slot) const noexcept { return is_med_ ? med_.importer().pcm_rate() : mod_.importer().pcm_rate(slot); }

bool ModuleImport::read(unsigned slot, std::uint32_t first, std::uint32_t n, std::int16_t* dst) noexcept {
  if (state_ != State::Done) {
    return false;
  }
  return (is_med_ ? med_.importer().read_pcm(slot, first, n, dst) : mod_.importer().read_pcm(slot, first, n, dst)).ok();
}

}  // namespace o3os::app
