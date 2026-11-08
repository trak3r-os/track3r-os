// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The module loader and the set of loaded modules (ADR 0002, OS-012a). Main loop only, in slices: begin() names a module, each
// step() reads about `budget` bytes of its file (one SD read at most a few KB), so a load never holds the main loop for more than
// a frame. The audio interrupt never sees a module that is not Done.
//
//   modules/instruments/<stem>.tmod      then
//   modules/effects/<stem>.tmod          the file of the module a project names (model::ModuleRef::name is the stem), then
//   modules/apps/<stem>.tmod             an app (OS-012b: ABI 1.1, header kind 4, launched from the MODULES screen), then
//   modules/importers/<stem>.tmod        an importer (OS-FLASHDIET I1: ABI 1.11, header kind 3, word 44 the layout tag). The first directory that has the stem wins, so a
//                                        stem is one module on a card. An importer is held apart from the playable modules (below): it never binds to a track and does not count against kMaxModules.
//
// BUILT-IN MODULES (OS-012d, new OS behaviour; builtin.h): the set may hold a table of .tmod files that live in flash. A stem in it is loaded exactly like a card file (the
// same loader, checks, relocation and arenas), read from flash instead of the card. OS-FLASHDIET A3: the table's file is stored compressed -- the 64-byte header raw, the rest a
// raw-deflate stream that this loader inflates through its own core::Inflater as it reads (read_some), so every check runs on the inflated bytes. The card is tried first; its copy is used when it is valid and prefers_card() (the same kind
// and ABI major, a module version above the built-in one's: OS-MODVER). Otherwise (no card, not mounted, no such file, a card error, a file that fails any check, one that is not
// newer) the loader starts over from the built-in copy, the card's file is not quarantined (the module works) and LoadedModule::card_status says why the card's copy was passed over.
// A stem with no built-in copy behaves as before.
//
// QUARANTINE (new OS behaviour, ADR 0002 "Containment"; the SD-module design guide): a module whose file fails validation (any Status that says the
// file is wrong: is_validation_failure) is quarantined for the rest of the boot: logged by the caller, never opened again until the next reset, the OS
// keeps running and the instrument that names it stays muted. The same set holds the modules the previous run died in (the platform's crash record
// names the module that was running: ModuleSet::quarantine with Status::Quarantined at boot, fault_id()). A missing file, a card error and "no room"
// are not quarantined: they may be fixed without a reboot.
//
// What a load does: open and read the 64-byte header, check_header(); take the hot part from the hot arena (the SRAM_L slab) and the
// cold part from the cold arena (SDRAM), or both from the cold arena when the slab has no room for the hot part (the module then runs
// from SDRAM, slower: `hot_in_sdram`, and the log says so); stream the image into place, then the relocation table, applying it as it
// arrives (image.h); check the CRC, read_desc(); call the platform's code-sync hook over what was written (data and instruction
// barriers, and the code cache's invalidate where the platform has one); record the module in the set. Any failure gives the arenas
// back to where they were and reports a Status; the files and the set stay as they were.
//
// No heap; the loader keeps a 256-byte buffer of relocation offsets and the header, so it is a static of its owner (SDRAM on the MK66:
// the SRAM_U budget has nothing left for it).

#include <o3os/core/inflate.h>
#include <o3os/hal/storage.h>
#include <o3os/module/arena.h>
#include <o3os/module/builtin.h>
#include <o3os/module/image.h>

#include <cstddef>
#include <cstdint>

namespace o3os::module {

inline constexpr std::uint32_t kStemMax = 31;  // model::kModuleNameMax

struct LoadedModule {
  char name[kStemMax + 1] = {};
  o3m_file_header header{};
  Desc32 desc{};
  DescExt32 ext{};  // ABI 1.8: the members past byte 64 of the descriptor (zeros below that minor; for a native entry, see entry)
  DescImp32 imp{};  // ABI 1.11 (OS-FLASHDIET I1): the member past byte 80, the importer's table (kind 3 only; zero otherwise)
  Placement place{};
  bool hot_in_sdram = false;  // the hot part is in the cold arena (the slab had no room)
  bool builtin = false;       // OS-012d: read from the firmware image (module/builtin.h), not from the card
  bool native_entry = false;  // OS-FLASHDIET I2: registered by add_native (no arenas of its own; drop_importer does not roll the arenas back for it)
  std::uint32_t hot_mark = 0; // OS-FLASHDIET I2: the hot arena's mark before this module was placed (drop_importer rolls back to it); 0 for a native entry
  Status card_status = Status::Ok;  // OS-012d: with `builtin`, why the card's copy was not used: NotFound (none, or no card), Older, or the refusal that its file earned; Ok when it was not tried
  std::uint32_t card_version = 0;   // OS-MODVER: with card_status == Older, the module version (O3M_VERSION; 0 unversioned) of the card's copy that was passed over; the built-in's is header.module_version
  // The descriptor as a native object: set when this process can call the module (a 32-bit target whose addresses are its pointers,
  // or a module registered as native); nullptr otherwise (the host tests read `desc` instead).
  const o3m_module* entry = nullptr;
  // OS-012b: the module's views can be drawn: ABI 1.1 or later (a 1.0 module's reserved table is validated and never called), a native entry, and a ui table.
  bool has_views() const noexcept { return header.abi_minor >= 1u && entry != nullptr && entry->ui != nullptr && entry->view_count != 0u; }
};

// The loaded modules, and the two arenas they and their instances' state live in.
class ModuleSet {
 public:
  // Both arenas must have been init()ed by the owner; the slab one may stay empty (every module then runs from SDRAM).
  Arena& hot() noexcept { return hot_; }
  Arena& cold() noexcept { return cold_; }
  // OS-PERF3: the arena the instruments' STATE blocks go to first. The render reads and writes an instrument's state every sample; on the MK66 the cold arena is SDRAM, 28 cycles a
  // load for the core against 10 for SRAM (bench mem), and a TB-303 instance (about 100 state accesses a sample) ran 2.7 times faster with its 380 bytes in SRAM_U. The platform gives this
  // arena a small SRAM_U buffer; without one (the host, a test, an arena that is full) the state comes from the cold arena as before.
  Arena& state() noexcept { return state_; }
  struct StateMark {
    std::uint32_t cold;
    std::uint32_t state;
  };
  StateMark state_mark() const noexcept { return StateMark{cold_.mark(), state_.mark()}; }
  void state_rollback(const StateMark& m) noexcept {
    cold_.rollback(m.cold);
    state_.rollback(m.state);
  }
  // `bytes` of 16-byte aligned state: the state arena first, then the cold one. False when neither has room.
  bool take_state(std::uint32_t bytes, std::uint8_t*& mem, std::uint32_t& addr) noexcept {
    return (state_.ready() && state_.take(bytes, 16, mem, addr)) || cold_.take(bytes, 16, mem, addr);
  }

  unsigned count() const noexcept { return count_; }
  const LoadedModule* at(unsigned i) const noexcept { return i < count_ ? &mods_[i] : nullptr; }
  // The index of the module with that name (case-sensitive), or -1. Importers are not in this table (see below), so a project's module ref never resolves to one: an importer never binds to a track.
  int find(const char* name) const noexcept;

  // OS-FLASHDIET I1: the importers (header kind 3, ABI 1.11) the set holds, apart from the playable modules: an importer does not count against kMaxModules, so a full set of
  // instruments still takes one, and find() above never returns it.
  static constexpr unsigned kMaxImporters = 4;
  unsigned importer_count() const noexcept { return importer_count_; }
  const LoadedModule* importer_at(unsigned i) const noexcept { return i < importer_count_ ? &importers_[i] : nullptr; }
  // The index of the importer with that name (case-sensitive), or -1.
  int find_importer(const char* name) const noexcept;
  // OS-FLASHDIET I2: forgets an importer (kind 3) by name: removed from the importer table and, when it was loaded from a file, its hot part's arena rolled back to the
  // mark the load began from (the cold part and the state block go back through the caller's state_rollback of the mark it took before the load). False when there is none.
  bool drop_importer(const char* name) noexcept;

  // ---- quarantine: stems refused for the rest of this boot (survives clear(): only a reset empties it). Names beyond kMaxQuarantined are
  // dropped (the oldest stay): a card with that many broken files has other problems.
  static constexpr unsigned kMaxQuarantined = 16;
  bool quarantined(const char* stem) const noexcept;
  // Adds `stem` with the reason; true when it is (now or already) in the list, false when the list is full or the stem is not a stem.
  bool quarantine(const char* stem, Status why) noexcept;
  unsigned quarantine_count() const noexcept { return qcount_; }
  const char* quarantine_name(unsigned i) const noexcept { return i < qcount_ ? q_[i].name : nullptr; }
  Status quarantine_reason(unsigned i) const noexcept { return i < qcount_ ? q_[i].why : Status::Ok; }

  // OS-012b: a module whose views went over budget (module/view.h) has them blocked for the rest of this boot: the page never calls them again. Cleared with the set
  // (a reset of the set gives the indices back); the quarantine below is what keeps the module from being loaded again.
  bool views_blocked(unsigned i) const noexcept { return i < kMaxModules && ((view_blocked_ >> i) & 1u) != 0u; }
  void block_views(unsigned i) noexcept {
    if (i < count_) {
      view_blocked_ = static_cast<std::uint16_t>(view_blocked_ | (1u << i));
    }
  }

  // OS-012d: the built-in modules (builtin.h): a table in flash that outlives the set, installed once by the image. Null/0 (the default): none.
  void set_builtins(const BuiltinModule* table, unsigned count) noexcept {
    builtins_ = table;
    builtin_count_ = count;
  }
  unsigned builtin_count() const noexcept { return builtin_count_; }
  const BuiltinModule* builtin_at(unsigned i) const noexcept { return i < builtin_count_ ? &builtins_[i] : nullptr; }
  // The built-in module with that stem (case-sensitive, like find()), or nullptr.
  const BuiltinModule* builtin(const char* stem) const noexcept;

  // Records a loaded module; false when the set is full or the name is taken. A kind 3 module (an importer, OS-FLASHDIET I1) goes to the importer table instead (kMaxImporters of them).
  bool add(const LoadedModule& m) noexcept;
  // A module that is compiled into this process (the host's shared objects, the built-in ones the OS may one day carry): no image, no
  // arenas, `entry` called directly. State blocks still come from the cold arena. `caps` (OS-H1) is the capability mask the module's file would carry (O3M_CAP_*; an app of ABI 1.3 only, else false).
  // A kind 3 entry (OS-FLASHDIET I1) needs ABI 1.11 and a non-NULL `importer` table, has no parameters and no views, and is recorded in the importer table, not in the module slots.
  bool add_native(const char* name, const o3m_module& entry, std::uint32_t caps = 0) noexcept;

  // Forgets every module and gives both arenas back. The caller has retired every instance first (audio/module_voices.h).
  void clear() noexcept {
    count_ = 0;
    importer_count_ = 0;
    view_blocked_ = 0;
    hot_.reset();
    cold_.reset();
    state_.reset();
  }

 private:
  struct Quarantine {
    char name[kStemMax + 1];
    Status why;
  };
  LoadedModule mods_[kMaxModules];
  unsigned count_ = 0;
  LoadedModule importers_[kMaxImporters];  // OS-FLASHDIET I1: the kind 3 modules, apart from mods_ (an importer does not count against kMaxModules)
  unsigned importer_count_ = 0;
  std::uint16_t view_blocked_ = 0;  // bit i: module i's views are blocked
  Quarantine q_[kMaxQuarantined] = {};
  unsigned qcount_ = 0;
  Arena hot_;
  Arena cold_;
  Arena state_;
  const BuiltinModule* builtins_ = nullptr;
  unsigned builtin_count_ = 0;
};

// The 16-bit module id the platform stamps into its crash context while module code runs (0x8000 | an FNV-1a hash of the stem, 15 bits; 0 means
// "no module"): after a fault the next boot compares it with the stems the project names. A collision quarantines an innocent module of the same
// project until the next boot; 1 in 32,768 per pair, and it only costs a reboot-and-reload (never the project).
std::uint16_t fault_id(const char* stem) noexcept;

// True for a file stem the loader accepts: 1..kStemMax characters, none of / \ : * ? " < > | or a control character, not "." or "..".
bool is_stem(const char* text) noexcept;

class ModuleLoader {
 public:
  enum class State : std::uint8_t { Idle, Running, Done, Failed };
  // What the platform does after bytes that will be executed were written: barriers, and the code cache's invalidate. May be null.
  using SyncFn = void (*)(const void* begin, std::uint32_t bytes);

  // Starts loading `stem` from the mounted card into `set`. False (state unchanged) when a load is running or the stem is not a stem
  // (status() says which, Busy or BadName). A file that is not there is not a refusal here: step() reports NotFound.
  // `importer` (OS-FLASHDIET I1/I2): the caller is the importer scan, so the stem is looked for in modules/importers/ only; without it the playable
  // directories are searched and modules/importers/ is skipped, so a project that names an importer's stem never loads the importer as a module.
  bool begin(hal::storage::Storage& storage, ModuleSet& set, const char* stem, SyncFn sync, bool importer = false) noexcept;
  // One slice, about `budget` bytes of file (at least 1). Returns the state after it.
  State step(std::size_t budget) noexcept;
  // Gives up a running load (the file is closed, the arenas rolled back).
  void cancel() noexcept;

  State state() const noexcept { return state_; }
  Status status() const noexcept { return status_; }
  // The module a Done load added; valid until the next begin().
  const LoadedModule& module() const noexcept { return mod_; }
  // For the log: bytes of the file read so far / in all.
  std::uint32_t read_bytes() const noexcept { return read_; }
  std::uint32_t file_bytes() const noexcept { return file_size_; }

 private:
  enum class Phase : std::uint8_t { Open, Header, Image, Relocs, Finish };
  // `fallback`: a failed card copy of a stem that has a built-in module restarts from the built-in one instead of failing the load (see the top).
  void fail(Status s, bool fallback = true) noexcept;
  void use_builtin(Status why) noexcept;  // starts (over) from the built-in copy of this stem; `why`: what was wrong with the card's
  bool read_some(void* dst, std::size_t want, std::size_t& got) noexcept;  // false: an error (fail() called)
  bool read_full(void* dst, std::size_t want, std::size_t& got) noexcept;  // until `want` bytes or the end of the file; false: an error
  void open_file() noexcept;
  void read_header() noexcept;
  std::size_t read_image(std::size_t budget) noexcept;
  std::size_t read_relocs(std::size_t budget) noexcept;
  void finish() noexcept;

  hal::storage::Storage* storage_ = nullptr;
  ModuleSet* set_ = nullptr;
  SyncFn sync_ = nullptr;
  bool want_importer_ = false;  // OS-FLASHDIET I1/I2: the caller is the importer scan (begin's `importer`): only modules/importers/ is searched
  hal::storage::FileHandle file_{};
  const BuiltinModule* builtin_ = nullptr;  // the built-in copy of this stem, if there is one
  bool from_flash_ = false;                 // the bytes come from builtin_, not the card
  std::uint32_t flash_pos_ = 0;
  Status card_status_ = Status::Ok;
  std::uint32_t card_version_ = 0;  // the version of the card copy that came out Older (the header it was read into is reset when the built-in one takes over)
  State state_ = State::Idle;
  Phase phase_ = Phase::Open;
  Status status_ = Status::Ok;
  char stem_[kStemMax + 1] = {};
  LoadedModule mod_{};
  std::uint32_t file_size_ = 0;
  std::uint32_t read_ = 0;       // bytes of the file consumed
  std::uint32_t pos_ = 0;        // bytes of the current section (image or table) consumed
  std::uint32_t crc_ = 0;
  std::uint32_t hot_mark_ = 0;   // arena marks to roll back to
  std::uint32_t cold_mark_ = 0;
  std::uint32_t offsets_[64] = {};
  Relocator reloc_{};
  // OS-FLASHDIET A3: the decoder of a built-in module's compressed rest (builtin.h z / z_size). One per loader: a load is stepped across passes, so it
  // cannot share another decoder. It lives here, in the module system's record on the MK66 (app_main.cpp's 64 KiB static_assert says whether it fits).
  core::Inflater inflater_;
};

}  // namespace o3os::module
