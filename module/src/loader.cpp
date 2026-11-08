// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/module/loader.h>

#include <o3os/model/import_layout.h>

#include <cstring>

namespace o3os::module {

namespace {

constexpr std::uint32_t kAlign = 16;

bool copy_text(char* dst, std::size_t cap, const char* src) noexcept {
  std::size_t n = 0;
  while (src[n] != '\0' && n + 1 < cap) {
    dst[n] = src[n];
    ++n;
  }
  dst[n] = '\0';
  return src[n] == '\0';
}

// A printable ASCII string of 1..limit-1 characters (a native entry's pointers are real ones).
bool text_ok(const char* text, std::size_t limit) noexcept {
  if (text == nullptr) {
    return false;
  }
  for (std::size_t i = 0; i < limit; ++i) {
    if (text[i] == '\0') {
      return i != 0u;
    }
    if (static_cast<unsigned char>(text[i]) < 0x20u || static_cast<unsigned char>(text[i]) > 0x7Eu) {
      return false;
    }
  }
  return false;
}

// ABI 1.12 (OS-INSFX-ABI2 A1): the rules read_desc applies to a file's fourth tail (image.cpp fx_ok), on a native entry: render_st an effect's only, fx_flags O3M_FX_WIDENS alone and with render_st, the KEY flag an
// effect's INT parameter 0..16 with no label entry (at most two), the presets an instrument's or an effect's with parameters only (Q8 a: an app and an importer carry them zero), at most 32, a name of 1..15
// printable characters and 1..param_count finite values inside their parameters' ranges. Called for a module that declares 1.12 or above, after the shape of the parameter table is known to be sound.
bool native_fx_ok(const o3m_module& e) noexcept {
  if ((e.render_st != nullptr && e.kind != O3M_KIND_EFFECT) || (e.fx_flags & ~fx_flags_allowed(e.kind, e.abi_minor)) != 0u || ((e.fx_flags & O3M_FX_WIDENS) != 0u && e.render_st == nullptr)) {
    return false;
  }
  std::uint32_t keys = 0;
  for (std::uint32_t i = 0; i < e.param_count; ++i) {
    const o3m_param& q = e.params[i];
    if ((q.flags & O3M_PARAM_KEY) != 0u && (e.kind != O3M_KIND_EFFECT || (q.flags & O3M_PARAM_INT) == 0u || q.min != 0.0f || q.max != 16.0f || ++keys > 2u ||
                                           (e.labels != nullptr && (e.labels[i].names != nullptr || e.labels[i].count != 0u)))) {
      return false;
    }
  }
  if ((e.presets == nullptr) != (e.preset_count == 0u) || e.preset_count > O3M_PRESETS_MAX) {
    return false;
  }
  if (e.preset_count == 0u) {
    return true;
  }
  if ((e.kind != O3M_KIND_INSTRUMENT && e.kind != O3M_KIND_EFFECT) || e.param_count == 0u) {
    return false;
  }
  for (std::uint32_t k = 0; k < e.preset_count; ++k) {
    const o3m_preset& pr = e.presets[k];
    if (!text_ok(pr.name, O3M_PRESET_NAME_MAX) || pr.values == nullptr || pr.count < 1u || pr.count > e.param_count) {
      return false;
    }
    for (std::uint32_t i = 0; i < pr.count; ++i) {
      const float v = pr.values[i];
      std::uint32_t bits;
      std::memcpy(&bits, &v, sizeof bits);
      if ((bits & 0x7F800000u) == 0x7F800000u || !(v >= e.params[i].min && v <= e.params[i].max)) {  // (NaN and infinity first: read as bits, safe under any float flags)
        return false;
      }
    }
  }
  return true;
}

}  // namespace

bool is_stem(const char* text) noexcept {
  if (text == nullptr) {
    return false;
  }
  std::size_t n = 0;
  for (; text[n] != '\0'; ++n) {
    const unsigned char c = static_cast<unsigned char>(text[n]);
    if (n >= kStemMax || c < 0x20 || c == 0x7F || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
      return false;
    }
  }
  return n != 0 && !(n == 1 && text[0] == '.') && !(n == 2 && text[0] == '.' && text[1] == '.');
}

std::uint16_t fault_id(const char* stem) noexcept {
  std::uint32_t h = 2166136261u;
  for (const char* c = stem; c != nullptr && *c != '\0'; ++c) {
    h = (h ^ static_cast<std::uint8_t>(*c)) * 16777619u;
  }
  return static_cast<std::uint16_t>(0x8000u | ((h ^ (h >> 15)) & 0x7FFFu));
}

bool ModuleSet::quarantined(const char* stem) const noexcept {
  for (unsigned i = 0; i < qcount_; ++i) {
    if (std::strcmp(q_[i].name, stem) == 0) {
      return true;
    }
  }
  return false;
}

bool ModuleSet::quarantine(const char* stem, Status why) noexcept {
  if (!is_stem(stem)) {
    return false;
  }
  if (quarantined(stem)) {
    return true;
  }
  if (qcount_ >= kMaxQuarantined) {
    return false;
  }
  (void)copy_text(q_[qcount_].name, sizeof q_[qcount_].name, stem);
  q_[qcount_].why = why;
  ++qcount_;
  return true;
}

namespace {
// ASCII case-insensitive equality: FAT and the ENGINE picker treat "TB303" and "tb303" as one stem, so the built-in matches either (OS-012d review).
unsigned lower(char c) noexcept {
  const unsigned u = static_cast<unsigned char>(c);
  return (u >= 'A' && u <= 'Z') ? u + 32u : u;
}

bool same_stem(const char* a, const char* b) noexcept {
  for (;; ++a, ++b) {
    const unsigned ca = lower(*a);
    const unsigned cb = lower(*b);
    if (ca != cb) {
      return false;
    }
    if (ca == 0u) {
      return true;
    }
  }
}
}  // namespace

const BuiltinModule* ModuleSet::builtin(const char* stem) const noexcept {
  for (unsigned i = 0; stem != nullptr && i < builtin_count_; ++i) {
    if (same_stem(builtins_[i].stem, stem)) {
      return &builtins_[i];
    }
  }
  return nullptr;
}

int ModuleSet::find(const char* name) const noexcept {
  if (name == nullptr) {
    return -1;
  }
  for (unsigned i = 0; i < count_; ++i) {
    if (std::strcmp(mods_[i].name, name) == 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int ModuleSet::find_importer(const char* name) const noexcept {
  if (name == nullptr) {
    return -1;
  }
  for (unsigned i = 0; i < importer_count_; ++i) {
    if (std::strcmp(importers_[i].name, name) == 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool ModuleSet::drop_importer(const char* name) noexcept {
  const int at = find_importer(name);
  if (at < 0) {
    return false;
  }
  const LoadedModule m = importers_[static_cast<unsigned>(at)];  // (the rollback marks are read before the table shifts)
  for (unsigned i = static_cast<unsigned>(at); i + 1u < importer_count_; ++i) {
    importers_[i] = importers_[i + 1u];
  }
  --importer_count_;
  if (!m.native_entry) {  // a native entry owns no arena memory; a loaded file's hot part goes back to the mark its load began from
    hot_.rollback(m.hot_mark);
  }
  return true;
}

bool ModuleSet::add(const LoadedModule& m) noexcept {
  if (!is_stem(m.name)) {
    return false;
  }
  if (m.header.kind == O3M_KIND_IMPORTER) {
    // OS-FLASHDIET I1: an importer (kind 3) is held apart from the playable modules: it does not count against kMaxModules and find() never returns it, so it never binds to a track.
    if (importer_count_ >= kMaxImporters || find_importer(m.name) >= 0 || find(m.name) >= 0) {
      return false;
    }
    importers_[importer_count_++] = m;
    return true;
  }
  if (count_ >= kMaxModules || find(m.name) >= 0 || find_importer(m.name) >= 0) {
    return false;
  }
  mods_[count_++] = m;
  return true;
}

bool ModuleSet::add_native(const char* name, const o3m_module& entry, std::uint32_t caps) noexcept {
  const bool importer = entry.kind == O3M_KIND_IMPORTER && entry.abi_minor >= kImporterAbiMinor;  // OS-FLASHDIET I1: kind 3 from ABI 1.11, with its table
  if (!is_stem(name) || entry.abi_major != kOsAbiMajor || entry.abi_minor > kOsAbiMinor || entry.struct_size != O3M_MODULE_SIZE(entry.abi_minor) ||
      !(entry.kind == O3M_KIND_INSTRUMENT || entry.kind == O3M_KIND_EFFECT || (entry.kind == O3M_KIND_APP && entry.abi_minor >= 1u && entry.view_count != 0u) ||
        (importer && entry.importer != nullptr)) ||
      entry.init == nullptr || entry.shutdown == nullptr ||
      (!(entry.kind == O3M_KIND_APP || importer) && (entry.render == nullptr || entry.set_param == nullptr || entry.note_on == nullptr || entry.note_off == nullptr)) ||
      (importer && (entry.param_count != 0u || entry.view_count != 0u || entry.ui != nullptr)) ||  // (an importer has no parameters and no views)
      entry.state_bytes == 0u || entry.state_bytes > kMaxStateBytes ||
      (entry.abi_minor >= kPitchAbiMinor && ((entry.pages == nullptr) != (entry.page_count == 0u) || entry.page_count > O3M_PAGES_MAX || (entry.param_count == 0u && entry.labels != nullptr))) ||  // (1.8: the page table and the label table are the module's own; the shape is checked, tests/module_builtin_test.cpp holds the built-in ones to every limit)
      entry.param_count > kMaxParams || (entry.param_count != 0u && entry.params == nullptr) || entry.view_count > kMaxViews || (entry.view_count == 0u) != (entry.ui == nullptr) || (entry.ui != nullptr && entry.ui->view_draw == nullptr) ||
      (caps != 0u && (entry.kind != O3M_KIND_APP || entry.abi_minor < 3u || (caps & ~static_cast<std::uint32_t>(entry.abi_minor >= 10u ? O3M_CAPS_ABI_1_10 : (entry.abi_minor >= 7u ? O3M_CAPS_ABI_1_7 : (entry.abi_minor >= 4u ? O3M_CAPS_ABI_1_4 : O3M_CAPS_ABI_1_3)))) != 0u))) {
    return false;
  }
  if (entry.abi_minor >= kStereoAbiMinor && !native_fx_ok(entry)) {
    return false;  // (the 1.12 members: render_st, fx_flags, presets and the KEY flag, like read_desc's fourth tail)
  }
  LoadedModule m;
  (void)copy_text(m.name, sizeof m.name, name);
  m.header.magic = O3M_FILE_MAGIC;
  m.header.header_size = O3M_FILE_HEADER_SIZE;
  m.header.target = O3M_TARGET_ARMV7EM;
  m.header.kind = static_cast<std::uint8_t>(entry.kind);
  m.header.abi_major = static_cast<std::uint8_t>(entry.abi_major);
  m.header.abi_minor = static_cast<std::uint8_t>(entry.abi_minor);
  m.header.state_bytes = entry.state_bytes;
  m.header.param_count = static_cast<std::uint16_t>(entry.param_count);
  m.header.view_count = static_cast<std::uint16_t>(entry.view_count);
  m.header.reserved2[0] = caps;  // OS-H1: the capabilities a file would have asked for
  m.header.reserved2[1] = importer ? model::kImportLayoutTag : 0u;  // OS-FLASHDIET I1: word 44 is the tag in a kind 3 file
  m.desc.abi_major = entry.abi_major;
  m.desc.abi_minor = entry.abi_minor;
  m.desc.kind = entry.kind;
  m.desc.struct_size = desc_size(entry.abi_minor);
  m.desc.state_bytes = entry.state_bytes;
  m.desc.param_count = entry.param_count;
  m.desc.view_count = entry.view_count;
  m.entry = &entry;
  m.native_entry = true;  // OS-FLASHDIET I2: a native entry owns no arena memory (drop_importer leaves the arenas alone for it)
  return add(m);
}

// ---- the loader -------------------------------------------------------------------------------------------------------------

bool ModuleLoader::begin(hal::storage::Storage& storage, ModuleSet& set, const char* stem, SyncFn sync, bool importer) noexcept {
  if (state_ == State::Running) {
    status_ = Status::Busy;
    return false;
  }
  if (!is_stem(stem)) {
    status_ = Status::BadName;
    return false;
  }
  if (set.quarantined(stem)) {
    status_ = Status::Quarantined;
    return false;
  }
  storage_ = &storage;
  set_ = &set;
  sync_ = sync;
  want_importer_ = importer;  // OS-FLASHDIET I1/I2: the importer scan looks in modules/importers/ only; every other caller skips it (a project that names an importer's stem never loads it)
  (void)copy_text(stem_, sizeof stem_, stem);
  mod_ = LoadedModule{};
  (void)copy_text(mod_.name, sizeof mod_.name, stem);
  file_ = hal::storage::FileHandle{};
  builtin_ = set.builtin(stem);
  from_flash_ = false;
  flash_pos_ = 0;
  card_status_ = Status::Ok;
  card_version_ = 0;
  file_size_ = read_ = pos_ = crc_ = 0;
  hot_mark_ = set.hot().mark();
  cold_mark_ = set.cold().mark();
  status_ = Status::Ok;
  phase_ = Phase::Open;
  state_ = State::Running;
  return true;
}

void ModuleLoader::use_builtin(Status why) noexcept {
  if (storage_ != nullptr && file_.is_open()) {
    storage_->close(file_);
  }
  set_->hot().rollback(hot_mark_);
  set_->cold().rollback(cold_mark_);
  mod_ = LoadedModule{};
  (void)copy_text(mod_.name, sizeof mod_.name, stem_);
  card_status_ = why;
  from_flash_ = true;
  flash_pos_ = 0;
  file_size_ = builtin_->size;
  inflater_.begin(builtin_->z, builtin_->z_size, builtin_->size - sizeof(o3m_file_header));  // OS-FLASHDIET A3: the rest of the file comes from the stream
  read_ = pos_ = crc_ = 0;
  status_ = Status::Ok;
  phase_ = Phase::Header;  // the state stays Running: the next step() reads the built-in header
}

void ModuleLoader::fail(Status s, bool fallback) noexcept {
  if (fallback && builtin_ != nullptr && !from_flash_ && set_ != nullptr) {
    use_builtin(s);  // the card's copy is wrong, or there is none: the built-in module, and the card's file is not blamed
    return;
  }
  if (storage_ != nullptr && file_.is_open()) {
    storage_->close(file_);
  }
  if (set_ != nullptr) {
    set_->hot().rollback(hot_mark_);
    set_->cold().rollback(cold_mark_);
  }
  if (set_ != nullptr && is_validation_failure(s)) {
    (void)set_->quarantine(stem_, s);
  }
  status_ = s;
  state_ = State::Failed;
}

void ModuleLoader::cancel() noexcept {
  if (state_ == State::Running) {
    fail(Status::Io, false);
    state_ = State::Idle;
  }
}

bool ModuleLoader::read_some(void* dst, std::size_t want, std::size_t& got) noexcept {
  if (from_flash_) {  // the same stepped read, from the image: the header raw, the rest inflated (OS-FLASHDIET A3)
    const std::size_t left = builtin_->size - flash_pos_;
    std::size_t n = want < left ? want : left;
    if (flash_pos_ < sizeof(o3m_file_header)) {
      const std::size_t hleft = sizeof(o3m_file_header) - flash_pos_;
      if (n > hleft) {
        n = hleft;
      }
      std::memcpy(dst, builtin_->data + flash_pos_, n);
    } else {
      n = inflater_.read(static_cast<std::uint8_t*>(dst), n);
      if (inflater_.failed()) {
        fail(Status::BadCrc);  // the image's own stream is broken (the tests inflate every built-in byte for byte)
        // A built-in stream is fixed at build time: only a broken image can corrupt it, so this is reported as BadCrc and the stem quarantined like any bad file.
        return false;
      }
    }
    flash_pos_ += static_cast<std::uint32_t>(n);
    got = n;
    return true;
  }
  std::size_t n = 0;
  const hal::storage::Status s = storage_->read(file_, dst, want, n);
  if (s != hal::storage::Status::Ok) {
    fail(Status::Io);
    return false;
  }
  got = n;
  return true;
}

bool ModuleLoader::read_full(void* dst, std::size_t want, std::size_t& got) noexcept {
  std::size_t have = 0;
  while (have < want) {
    std::size_t n = 0;
    if (!read_some(static_cast<std::uint8_t*>(dst) + have, want - have, n)) {
      return false;
    }
    if (n == 0u) {
      break;  // the end of the file
    }
    have += n;
  }
  got = have;
  return true;
}

void ModuleLoader::open_file() noexcept {
  char path[24 + kStemMax + 8];
  // OS-FLASHDIET I1/I2 (review note 1): modules/importers/ is searched only when the caller is the importer scan (`want_importer_`); a project that names an
  // importer's stem must not load the importer, so the playable directories are searched without it and the first directory that has the stem wins as before.
  static constexpr const char* kPlayableDirs[] = {"modules/instruments/", "modules/effects/", "modules/apps/"};
  static constexpr const char* kImporterDir = "modules/importers/";
  const char* const* dirs = want_importer_ ? &kImporterDir : kPlayableDirs;
  const unsigned dir_count = want_importer_ ? 1u : 3u;
  bool found = false;
  for (unsigned d = 0; d < dir_count && !found; ++d) {
    const char* dir = dirs[d];
    std::size_t n = 0;
    for (const char* c = dir; *c != '\0'; ++c) {
      path[n++] = *c;
    }
    for (const char* c = stem_; *c != '\0'; ++c) {
      path[n++] = *c;
    }
    for (const char* c = ".tmod"; *c != '\0'; ++c) {
      path[n++] = *c;
    }
    path[n] = '\0';
    const hal::storage::Status s = storage_->open(path, file_);
    if (s == hal::storage::Status::Ok) {
      found = true;
      break;
    }
    if (s != hal::storage::Status::NotFound && s != hal::storage::Status::NotADirectory && s != hal::storage::Status::NotAFile) {
      // No card and a card that is not mounted are "nothing there" for a stem that has a built-in module (which then takes over); any other failure is the card's.
      const bool absent = builtin_ != nullptr && (s == hal::storage::Status::NoCard || s == hal::storage::Status::NotMounted);
      fail(absent ? Status::NotFound : Status::Io);
      return;
    }
  }
  if (!found) {
    fail(Status::NotFound);
    return;
  }
  if (storage_->size(file_, file_size_) != hal::storage::Status::Ok) {
    fail(Status::Io);
    return;
  }
  phase_ = Phase::Header;
}

void ModuleLoader::read_header() noexcept {
  if (file_size_ < sizeof(o3m_file_header)) {
    fail(Status::TooShort);
    return;
  }
  std::size_t got = 0;
  if (!read_full(&mod_.header, sizeof(o3m_file_header), got)) {
    return;
  }
  if (got != sizeof(o3m_file_header)) {
    fail(Status::Io);  // the file is shorter than its size said
    return;
  }
  read_ = sizeof(o3m_file_header);
  const o3m_file_header& h = mod_.header;
  const Status check = check_header(h, file_size_);
  if (check != Status::Ok) {
    fail(check);
    return;
  }
  if (!from_flash_ && builtin_ != nullptr && !prefers_card(h, builtin_header(*builtin_))) {
    card_version_ = h.module_version;  // (h is mod_.header, which use_builtin() resets)
    use_builtin(Status::Older);  // a valid card copy that is not newer than the built-in module (or another kind): not used
    return;
  }
  // Placement: the hot part in the slab, the cold part in SDRAM; no room for the hot part in the slab: all of it in SDRAM, hot first.
  const std::uint32_t cold = h.image_size - h.hot_size;
  Placement& p = mod_.place;
  bool hot_ok = true;
  if (h.hot_size != 0u) {
    hot_ok = set_->hot().ready() && set_->hot().take(h.hot_size, kAlign, p.hot_mem, p.hot_addr);
  }
  if (hot_ok) {
    if (!set_->cold().take(cold, kAlign, p.cold_mem, p.cold_addr)) {
      fail(Status::NoMemory);
      return;
    }
  } else {
    mod_.hot_in_sdram = true;
    std::uint8_t* both = nullptr;
    std::uint32_t addr = 0;
    if (!set_->cold().take(h.image_size, kAlign, both, addr)) {
      fail(Status::NoMemory);
      return;
    }
    p.hot_mem = both;
    p.hot_addr = addr;
    p.cold_mem = both + h.hot_size;
    p.cold_addr = addr + h.hot_size;
  }
  if (h.hot_size == 0u) {
    p.hot_mem = nullptr;
    p.hot_addr = 0;
  }
  o3m_file_header zeroed = h;  // the CRC covers the header with its own field read as zero, then the image, then the table
  zeroed.image_crc = 0;
  crc_ = crc32_update(0, &zeroed, sizeof zeroed);
  pos_ = 0;
  phase_ = Phase::Image;
}

std::size_t ModuleLoader::read_image(std::size_t budget) noexcept {
  const o3m_file_header& h = mod_.header;
  std::size_t used = 0;
  while (used < budget && pos_ < h.image_size) {
    std::uint8_t* dst;
    std::uint32_t room;
    if (pos_ < h.hot_size) {
      dst = mod_.place.hot_mem + pos_;
      room = h.hot_size - pos_;
    } else {
      dst = mod_.place.cold_mem + (pos_ - h.hot_size);
      room = h.image_size - pos_;
    }
    std::size_t want = budget - used < room ? budget - used : room;
    std::size_t got = 0;
    if (!read_some(dst, want, got)) {
      return used;
    }
    if (got == 0u) {
      fail(Status::Io);
      return used;
    }
    crc_ = crc32_update(crc_, dst, got);
    pos_ += static_cast<std::uint32_t>(got);
    read_ += static_cast<std::uint32_t>(got);
    used += got;
  }
  if (pos_ == h.image_size) {
    reloc_.begin(h, mod_.place);
    pos_ = 0;
    phase_ = Phase::Relocs;
  }
  return used;
}

std::size_t ModuleLoader::read_relocs(std::size_t budget) noexcept {
  const o3m_file_header& h = mod_.header;
  std::size_t used = 0;
  const std::uint32_t total = h.reloc_count * 4u;
  while (pos_ < total && (used < budget || used == 0u)) {
    std::size_t want = total - pos_;
    want = want < sizeof offsets_ ? want : sizeof offsets_;
    if (used != 0u && want > budget - used) {
      want = (budget - used) & ~std::size_t{3};
      if (want == 0u) {
        break;
      }
    }
    std::size_t got = 0;
    if (!read_full(offsets_, want, got)) {
      return used;
    }
    if (got != want) {
      fail(Status::Io);  // the file ended inside its own table
      return used;
    }
    crc_ = crc32_update(crc_, offsets_, got);
    const Status s = reloc_.apply(offsets_, static_cast<std::uint32_t>(got / 4u));
    if (s != Status::Ok) {
      fail(s);
      return used;
    }
    pos_ += static_cast<std::uint32_t>(got);
    read_ += static_cast<std::uint32_t>(got);
    used += got;
  }
  if (pos_ == total) {
    phase_ = Phase::Finish;
  }
  return used;
}

void ModuleLoader::finish() noexcept {
  const o3m_file_header& h = mod_.header;
  if (read_ != file_size_ || !reloc_.complete()) {
    fail(Status::BadFileLength);
    return;
  }
  if (crc_ != h.image_crc) {
    fail(Status::BadCrc);
    return;
  }
  const Status d = read_desc(h, mod_.place, mod_.desc, &mod_.ext, &mod_.imp);
  if (d != Status::Ok) {
    fail(d);
    return;
  }
  if (sync_ != nullptr) {
    if (h.hot_size != 0u) {
      sync_(mod_.place.hot_mem, h.hot_size);
    }
    sync_(mod_.place.cold_mem, h.image_size - h.hot_size);
  }
  // A native entry exists where a target address is a pointer of this process (the MK66: the placement's memory is at its address).
  if (sizeof(void*) == 4u && reinterpret_cast<std::uintptr_t>(mod_.place.cold_mem) == mod_.place.cold_addr) {
    mod_.entry = reinterpret_cast<const o3m_module*>(mod_.place.cold_mem + (h.desc_offset - h.hot_size));
  }
  mod_.builtin = from_flash_;
  mod_.card_status = from_flash_ ? card_status_ : Status::Ok;
  mod_.card_version = from_flash_ && card_status_ == Status::Older ? card_version_ : 0u;
  mod_.hot_mark = hot_mark_;  // OS-FLASHDIET I2: what drop_importer rolls the hot arena back to
  if (!set_->add(mod_)) {
    fail(Status::NoMemory);  // the set is full or the name is taken (fail() closes the file)
    return;
  }
  storage_->close(file_);
  status_ = Status::Ok;
  state_ = State::Done;
}

ModuleLoader::State ModuleLoader::step(std::size_t budget) noexcept {
  if (state_ != State::Running) {
    return state_;
  }
  budget = budget == 0u ? 1u : budget;
  switch (phase_) {
    case Phase::Open:
      open_file();
      break;
    case Phase::Header:
      read_header();
      break;
    case Phase::Image:
      (void)read_image(budget);
      break;
    case Phase::Relocs:
      (void)read_relocs(budget);
      break;
    case Phase::Finish:
      finish();
      break;
  }
  return state_;
}

}  // namespace o3os::module
