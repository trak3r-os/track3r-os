// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/module_binder.h>

#include <o3os/audio/diag.h>

#include <cstring>

namespace o3os::app {

// OS-INSFX S3b: the host never holds more inserts than the project can store (a console bind writes the model). The chunk's cap is the format's: raising it is a minor bump (docs/formats/o3-project.md).
static_assert(audio::gov::kMaxInserts <= model::kMaxInserts, "every insert the host can hold has a record in the project");

namespace {

using audio::ModuleVoices;
using Kind = ModuleProblem::Kind;

constexpr std::uint8_t kModuleProblem = 0xFF;

const char* bind_name(ModuleVoices::Bind b) noexcept {
  switch (b) {
    case ModuleVoices::Bind::Ok: return "ok";
    case ModuleVoices::Bind::NoScratch: return "no output blocks";
    case ModuleVoices::Bind::BadSlot: return "bad slot";
    case ModuleVoices::Bind::SlotTaken: return "slot already bound";
    case ModuleVoices::Bind::NoInstance: return "too many module instruments";
    case ModuleVoices::Bind::BadState: return "bad state block";
    case ModuleVoices::Bind::InitFailed: return "init() refused";
  }
  return "?";
}

const char* insert_bind_name(audio::InsertHost::Bind b) noexcept {
  using B = audio::InsertHost::Bind;
  switch (b) {
    case B::Ok: return "ok";
    case B::NoGraph: return "no insert host";
    case B::BadWhere: return "bad slot";
    case B::WhereTaken: return "slot already bound";
    case B::BadModule: return "not an effect this build can run";
    case B::NoInstance: return "too many inserts";
    case B::NeedState:
    case B::BadState: return "bad state block";
    case B::InitFailed: return "init() refused";
    case B::NoCpu: return "no CPU";
  }
  return "?";
}

void copy_name(char* dst, const char* src) noexcept {
  std::size_t n = 0;
  while (src[n] != '\0' && n < module::kStemMax) {
    dst[n] = src[n];
    ++n;
  }
  dst[n] = '\0';
}

}  // namespace

void ModuleBinder::note(Kind kind, unsigned slot, const char* name, module::Status status, ModuleVoices::Bind bind, bool insert, unsigned islot,
                        audio::InsertHost::Bind ibind) noexcept {
  if (report_.problem_count >= ModuleReport::kMaxProblems) {
    ++report_.problems_dropped;
    return;
  }
  ModuleProblem& p = report_.problems[report_.problem_count++];
  p.kind = kind;
  p.slot = static_cast<std::uint8_t>(slot);
  p.insert = insert;
  p.islot = static_cast<std::uint8_t>(islot);
  p.status = status;
  p.bind = bind;
  p.ibind = ibind;
  copy_name(p.name, name);
}

bool ModuleBinder::begin(hal::storage::Storage& storage, const model::Project& project, module::ModuleSet& set, module::ModuleLoader& loader,
                         audio::ModuleVoices& voices, module::ModuleLoader::SyncFn sync, audio::InsertHost* inserts) noexcept {
  if (state_ == State::Running) {
    return false;
  }
  storage_ = &storage;
  project_ = &project;
  set_ = &set;
  loader_ = &loader;
  voices_ = &voices;
  inserts_ = inserts;
  sync_ = sync;
  report_ = ModuleReport{};
  for (std::uint32_t i = 0; i < model::kMaxInstruments; ++i) {
    const model::Instrument* in = project.instrument(i);
    report_.instruments += (in != nullptr && in->kind == model::InstrumentKind::Module) ? 1u : 0u;
  }
  report_.inserts = inserts_ != nullptr ? project.inserts().count : 0u;
  for (std::uint32_t m = 0; m < project.module_count(); ++m) {
    report_.modules += wanted(m) ? 1u : 0u;
  }
  cursor_ = 0;
  loading_ = false;
  phase_ = Phase::Load;
  state_ = State::Running;
  return true;
}

// The module table's entry `module_index` is named by a Module instrument.
bool ModuleBinder::named_by_instrument(unsigned module_index) const noexcept {
  for (std::uint32_t i = 0; i < model::kMaxInstruments; ++i) {
    const model::Instrument* in = project_->instrument(i);
    if (in != nullptr && in->kind == model::InstrumentKind::Module && in->module_index == module_index) {
      return true;
    }
  }
  return false;
}

// ... or by a (non-master) insert record, when the binder owns the inserts.
bool ModuleBinder::named_by_insert(unsigned module_index) const noexcept {
  if (inserts_ == nullptr) {
    return false;
  }
  const model::InsertState& s = project_->inserts();
  for (std::uint32_t i = 0; i < s.count; ++i) {
    if (s.rec[i].where != model::kInsertWhereMaster && s.rec[i].module == module_index) {
      return true;
    }
  }
  return false;
}

void ModuleBinder::load_next(std::size_t budget) noexcept {
  if (cursor_ >= project_->module_count()) {
    phase_ = Phase::Bind;
    cursor_ = 0;
    return;
  }
  const model::ModuleRef& ref = *project_->module(cursor_);
  if (!loading_) {
    if (!wanted(cursor_)) {
      ++cursor_;
      return;
    }
    if (set_->find(ref.name) >= 0) {
      ++report_.already;
      ++cursor_;
      return;
    }
    if (!loader_->begin(*storage_, *set_, ref.name, sync_)) {
      note(loader_->status() == module::Status::Quarantined ? Kind::Quarantined : Kind::Refused, kModuleProblem, ref.name, loader_->status(),
           ModuleVoices::Bind::Ok, !named_by_instrument(cursor_));
      ++cursor_;
      return;
    }
    loading_ = true;
  }
  const module::ModuleLoader::State s = loader_->step(budget);
  if (s == module::ModuleLoader::State::Running) {
    return;
  }
  loading_ = false;
  if (s == module::ModuleLoader::State::Done) {
    ++report_.loaded;
    const module::LoadedModule& m = loader_->module();
    report_.abi_differs += (ref.abi_major != 0u && ref.abi_major != m.header.abi_major) ? 1u : 0u;
  } else {
    const module::Status why = loader_->status();
    Kind k = Kind::Refused;
    if (why == module::Status::NotFound) {
      k = Kind::NotFound;
    } else if (why == module::Status::NoMemory) {
      k = Kind::NoRoom;
    } else if (why == module::Status::Io) {
      k = Kind::CardError;
    }
    note(k, kModuleProblem, ref.name, why, ModuleVoices::Bind::Ok, !named_by_instrument(cursor_));
  }
  ++cursor_;
}

void ModuleBinder::bind_next() noexcept {
  while (cursor_ < model::kMaxInstruments) {
    const unsigned slot = cursor_++;
    const model::Instrument* in = project_->instrument(slot);
    if (in == nullptr || in->kind != model::InstrumentKind::Module) {
      continue;
    }
    const model::ModuleRef* ref = project_->module(in->module_index);
    const int idx = ref != nullptr ? set_->find(ref->name) : -1;
    if (idx < 0) {
      ++report_.muted;  // the module's own problem is in the list already (NotFound, Refused ...)
      continue;
    }
    const module::LoadedModule& m = *set_->at(static_cast<unsigned>(idx));
    if (m.header.kind != O3M_KIND_INSTRUMENT) {
      note(Kind::WrongKind, slot, ref->name, module::Status::BadKind, ModuleVoices::Bind::Ok);
      ++report_.muted;
      return;
    }
    if (m.entry == nullptr) {
      note(Kind::NotRunnable, slot, ref->name, module::Status::Ok, ModuleVoices::Bind::Ok);
      ++report_.muted;
      return;
    }
    const module::ModuleSet::StateMark mark = set_->state_mark();
    std::uint8_t* mem = nullptr;
    std::uint32_t addr = 0;
    if (!set_->take_state(m.entry->state_bytes, mem, addr)) {
      note(Kind::NoRoom, slot, ref->name, module::Status::NoMemory, ModuleVoices::Bind::Ok);
      ++report_.muted;
      return;
    }
    std::memset(mem, 0, m.entry->state_bytes);
    const ModuleVoices::Bind b = voices_->bind(slot, *m.entry, module::fault_id(ref->name), mem, in->params, in->param_len);
    if (b != ModuleVoices::Bind::Ok) {
      set_->state_rollback(mark);
      note(Kind::BindFailed, slot, ref->name, module::Status::Ok, b);
      ++report_.muted;
      return;
    }
    ++report_.bound;
    return;  // one instrument a step: its init() may take a while
  }
  if (inserts_ != nullptr && project_->inserts().count != 0u) {
    phase_ = Phase::Insert;
    cursor_ = 0;
    return;
  }
  state_ = State::Done;
}

// OS-INSFX S3b: one insert record a step (its init() may take a while). A record that cannot be bound is skipped and the run goes on: the load never fails for an insert.
void ModuleBinder::bind_insert_next() noexcept {
  const model::InsertState& list = project_->inserts();
  while (cursor_ < list.count) {
    const model::InsertRecord& r = list.rec[cursor_++];
    if (r.where == model::kInsertWhereMaster) {
      ++report_.inserts_master;  // S6 binds the master's
      continue;
    }
    const model::ModuleRef* ref = project_->module(r.module);
    const int idx = ref != nullptr ? set_->find(ref->name) : -1;
    if (idx < 0) {
      ++report_.inserts_skipped;  // the module's own problem is in the list already (NotFound, Refused ...)
      continue;
    }
    const unsigned where = r.where;
    const module::LoadedModule& m = *set_->at(static_cast<unsigned>(idx));
    if (m.header.kind != O3M_KIND_EFFECT) {
      note(Kind::WrongKind, where, ref->name, module::Status::BadKind, ModuleVoices::Bind::Ok, true, r.slot);
      ++report_.inserts_skipped;
      return;
    }
    if (m.entry == nullptr) {
      note(Kind::NotRunnable, where, ref->name, module::Status::Ok, ModuleVoices::Bind::Ok, true, r.slot);
      ++report_.inserts_skipped;
      return;
    }
    const unsigned key = where * 2u + r.slot;  // the host's key: track * 2 + slot
    const std::uint8_t* blob = reinterpret_cast<const std::uint8_t*>(r.param);
    const std::uint32_t blob_len = 4u * r.n;
    const std::uint16_t fault = module::fault_id(ref->name);
    const module::ModuleSet::StateMark mark = set_->state_mark();
    bool took = false;
    audio::InsertHost::Bind b = inserts_->bind(key, *m.entry, fault, nullptr, blob, blob_len, true);  // a free record that held this module keeps its state block
    if (b == audio::InsertHost::Bind::NeedState) {
      std::uint8_t* mem = nullptr;
      std::uint32_t addr = 0;
      if (!set_->take_state(m.entry->state_bytes, mem, addr)) {
        note(Kind::NoRoom, where, ref->name, module::Status::NoMemory, ModuleVoices::Bind::Ok, true, r.slot);
        ++report_.inserts_skipped;
        return;
      }
      took = true;
      std::memset(mem, 0, m.entry->state_bytes);
      b = inserts_->bind(key, *m.entry, fault, mem, blob, blob_len, true);
    }
    if (b != audio::InsertHost::Bind::Ok) {
      if (took) {
        set_->state_rollback(mark);
      }
      note(Kind::BindFailed, where, ref->name, module::Status::Ok, ModuleVoices::Bind::Ok, true, r.slot, b);
      ++report_.inserts_skipped;
      return;
    }
    if ((r.flags & model::kInsertFlagBypass) != 0u) {
      (void)inserts_->set_bypass(key, true, true);  // bound, and heard dry: the flag is the user's
    }
    ++report_.inserts_bound;
    return;
  }
  state_ = State::Done;
}

ModuleBinder::State ModuleBinder::step(std::size_t budget) noexcept {
  if (state_ != State::Running) {
    return state_;
  }
  if (phase_ == Phase::Load) {
    load_next(budget);
  } else if (phase_ == Phase::Bind) {
    bind_next();
  } else {
    bind_insert_next();
  }
  return state_;
}

std::size_t describe_modules(const ModuleReport& r, char* out, std::size_t cap) noexcept {
  audio::diag::Text t(out, cap);
  t.str("modules: ").dec(r.instruments).str(" instruments, ").dec(r.loaded).str(" loaded");
  if (r.already != 0u) {
    t.str(", ").dec(r.already).str(" already there");
  }
  t.str(", ").dec(r.bound).str(" bound, ").dec(r.muted).str(" muted");
  if (r.abi_differs != 0u) {
    t.str(", ").dec(r.abi_differs).str(" saved with another ABI");
  }
  if (r.inserts != 0u) {
    t.str("; inserts: ").dec(r.inserts).str(", ").dec(r.inserts_bound).str(" bound, ").dec(r.inserts_skipped).str(" skipped");
    if (r.inserts_master != 0u) {
      t.str(", ").dec(r.inserts_master).str(" master (not bound yet)");
    }
  }
  return t.size();
}

std::size_t describe_module_problem(const ModuleProblem& p, char* out, std::size_t cap) noexcept {
  audio::diag::Text t(out, cap);
  t.str("mod: ");
  if (p.slot != kModuleProblem) {
    if (p.insert) {
      t.str("insert track ").dec(static_cast<std::uint32_t>(p.slot) + 1u).str(" slot ").dec(static_cast<std::uint32_t>(p.islot) + 1u).str(" skipped: ");
    } else {
      t.str("instrument ").dec(static_cast<std::uint32_t>(p.slot) + 1u).str(" muted: ");
    }
  }
  t.str(p.name).chr(' ');
  switch (p.kind) {
    case Kind::NotFound: t.str(p.insert ? "not found (modules/effects/" : "not found (modules/instruments/").str(p.name).str(".tmod)"); break;
    case Kind::Refused: t.str("refused: ").str(module::status_name(p.status)).str(", quarantined until reboot"); break;
    case Kind::Quarantined: t.str("quarantined until reboot"); break;
    case Kind::NoRoom: t.str("does not fit the module memory"); break;
    case Kind::CardError: t.str("could not be read from the card"); break;
    case Kind::WrongKind: t.str(p.insert ? "is not an effect" : "is not an instrument"); break;
    case Kind::NotRunnable: t.str("cannot run in this build"); break;
    case Kind::BindFailed: t.str("did not start: ").str(p.insert ? insert_bind_name(p.ibind) : bind_name(p.bind)); break;
  }
  return t.size();
}

}  // namespace o3os::app
