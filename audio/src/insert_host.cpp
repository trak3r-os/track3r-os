// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/audio/insert_host.h>

#include <o3os/audio/kernels.h>
#include <o3os/audio/module_q15.h>

#include <cstring>

namespace o3os::audio {

namespace {

constexpr unsigned wet_of(float mix) noexcept {
  return mix <= 0.0f ? 0u : (mix >= 1.0f ? 255u : static_cast<unsigned>(mix * 255.0f + 0.5f));
}

bool block_silent(const std::int16_t* io, std::uint32_t n) noexcept {  // a word at a time (memcpy: no alignment is assumed); n is even for every block the graph passes
  std::uint32_t any = 0;
  std::uint32_t i = 0;
  for (; i + 2u <= n; i += 2u) {
    std::uint32_t w;
    std::memcpy(&w, io + i, 4);
    any |= w;
  }
  for (; i < n; ++i) {
    any |= static_cast<std::uint16_t>(io[i]);
  }
  return any == 0u;
}

// S6: a float plane of the master chain is silent when every sample is +0.0 or -0.0 (the bits, OR-ed a word at a time: no float compare, no NaN trap).
bool plane_silent(const float* p, std::uint32_t n) noexcept {
  std::uint32_t any = 0;
  for (std::uint32_t i = 0; i < n; ++i) {
    std::uint32_t w;
    std::memcpy(&w, p + i, 4);
    any |= w & 0x7FFFFFFFu;
  }
  return any == 0u;
}

// S6: the master insert's scrub: a NaN or an infinity (the exponent field all ones) is 0. Written on the bits, so it holds whatever the compiler thinks of NaN. Returns the samples it changed.
std::uint32_t scrub_plane(float* p, std::uint32_t n) noexcept {
  std::uint32_t bad = 0;
  for (std::uint32_t i = 0; i < n; ++i) {
    std::uint32_t w;
    std::memcpy(&w, p + i, 4);
    if ((w & 0x7F800000u) == 0x7F800000u) {
      p[i] = 0.0f;
      ++bad;
    }
  }
  return bad;
}

// The render's other work (the aggregate net's input): the fast mean follows a sustained rise in tens of blocks (1/32 a block), the slow one is the base's rate of fall (1/256); the net judges the slow mean
// unless the fast one is more than the deadband above it (a spike moves the fast mean by a few percent at most). Both are plain running averages for their first blocks (the divisor is the block count until it
// reaches 32 or 256), so a first cold block is forgotten as fast as the data comes in.
constexpr std::uint32_t kOthersFastBlocks = 32;
constexpr std::uint32_t kOthersSlowBlocks = std::uint32_t{1} << gov::kWorstDecayShift;
constexpr unsigned kOthersDeadbandPercent = 5;

constexpr std::uint32_t running_mean(std::uint32_t mean, std::uint32_t sample, std::uint32_t n, std::uint32_t cap) noexcept {
  const std::uint32_t d = n < cap ? n : cap;
  return sample >= mean ? mean + (sample - mean) / d : mean - (mean - sample) / d;
}

}  // namespace

InsertHost::InsertHost() noexcept {
  for (unsigned i = 0; i < kInstances; ++i) {
    in_[i].host = this;
  }
  for (unsigned w = 0; w < kWhere; ++w) {
    wh_[w] = kNone;
    rwh_[w] = kNone;
  }
}

// ---- main loop ------------------------------------------------------------------------------------------------------------------------------

bool InsertHost::push(const Command& c) noexcept {
  if (ring_.push(c)) {
    return true;
  }
  ++stats_.ring_full;
  return false;
}

bool InsertHost::retired_ok(const Inst& in) const noexcept {
  return in.retired_gen.load(std::memory_order_acquire) == in.gen &&
         static_cast<std::uint32_t>(blocks_.load(std::memory_order_acquire) - in.retired_blk.load(std::memory_order_relaxed)) >= 2u;
}

bool InsertHost::live(const Inst& in) const noexcept {
  return in.ml == Ml::Bound && in.act_pushed && in.pub_gen.load(std::memory_order_acquire) == in.gen;
}

// The aggregate net's rule, asked in advance: the render's other work (`others_pub_`, the module voices' included) plus the inserts that run (their estimates) plus this one must stay under the threshold and under
// the instance budget, so a bind or an un-bypass is admitted exactly when the net would not force it. `cost` 0 is an unmeasured candidate: a first bind (`first_bind`) counts it as 0 (the nets judge it once it
// runs: spec 6), an un-bypass assumes the cold figure.
bool InsertHost::admit(std::uint32_t cost, bool first_bind, unsigned copies) const noexcept {
  const gov::Governor& g = voices_->governor();
  std::uint32_t all = cost != 0u ? cost : (first_bind ? 0u : gov::kColdInsertCycles * copies);  // (`cost` is the candidate's whole figure: both instances of a master insert)
  for (unsigned i = 0; i < kInstances; ++i) {
    const Inst& in = in_[i];
    if (in.ml == Ml::Bound && !in.byp_want && in.forced.load(std::memory_order_relaxed) == 0u) {  // it runs, or it will once the render takes it
      const std::uint32_t est = in.est.load(std::memory_order_relaxed);
      all += est != 0u ? est : gov::kColdInsertCycles;
    }
  }
  return all <= g.budget() && gov::decide(others_pub_.load(std::memory_order_relaxed), all, false, g.threshold()) == gov::Verdict::Admit;
}

[[gnu::noinline]] std::uint16_t InsertHost::enter(const Inst& in) noexcept {
  return voices_->hooks().enter != nullptr ? voices_->hooks().enter(in.fault_id) : std::uint16_t{0};
}

[[gnu::noinline]] void InsertHost::leave(std::uint16_t previous) noexcept {
  if (voices_->hooks().enter != nullptr) {
    voices_->hooks().enter(previous);
  }
}

// A running instance starts again from its fresh state (un-bypass, the start of an export, a leave that rebinds).
bool InsertHost::restart_module(Inst& in) noexcept {
  shutdown_module(in);
  if (!init_module(in)) {
    return false;
  }
  apply_blob(in);
  return true;
}

bool InsertHost::init_module(Inst& in) noexcept {
  std::memset(in.state, 0, in.mod->state_bytes);
  const std::uint16_t prev = enter(in);
  const int rc = in.mod->init(in.state, &voices_->host());
  leave(prev);
  in.shut = rc != 0;
  return rc == 0;
}

void InsertHost::shutdown_module(Inst& in) noexcept {
  if (!in.shut && in.mod != nullptr && in.state != nullptr) {
    const std::uint16_t prev = enter(in);
    in.mod->shutdown(in.state);
    leave(prev);
  }
  in.shut = true;
}

// The stored values again, after an init().
void InsertHost::apply_blob(Inst& in) noexcept {
  const unsigned idx = static_cast<unsigned>(&in - in_);
  const std::uint32_t n = in.mod->param_count < kParams ? in.mod->param_count : kParams;
  if (in.mod->set_param == nullptr) {
    return;
  }
  const std::uint16_t prev = enter(in);
  for (std::uint32_t i = 0; i < n; ++i) {
    if (((in.set_mask >> i) & 1u) != 0u) {
      in.mod->set_param(in.state, i, (*blobs_)[idx][i]);
    }
  }
  leave(prev);
  in.dirty = 0;
}

// A new binding of an initialised instance: a new generation, nothing pushed yet.
void InsertHost::start_binding(Inst& in, unsigned where, const std::uint8_t* params, std::uint32_t param_len) noexcept {
  const unsigned idx = static_cast<unsigned>(&in - in_);
  in.gen = next_gen(in.gen);
  in.cur_gen.store(in.gen, std::memory_order_relaxed);  // (the Activate is pushed after this: release)
  in.retired_gen.store(0, std::memory_order_relaxed);   // a generation that wrapped round must not match the retirement of the binding it shares its number with
  in.where = static_cast<std::uint8_t>(where);
  in.act_pushed = false;
  in.deact_pushed = false;
  in.byp_want = false;
  in.byp_pushed = false;
  in.then_reactivate = false;
  in.dirty = 0;
  in.forced.store(0, std::memory_order_relaxed);
  in.ml = Ml::Bound;
  wh_[where] = static_cast<std::uint8_t>(idx);
  if (params != nullptr && in.mod->set_param != nullptr) {
    const std::uint32_t cap = in.mod->param_count < kParams ? in.mod->param_count : kParams;
    const std::uint32_t n = cap < param_len / 4u ? cap : param_len / 4u;
    const std::uint16_t prev = enter(in);
    for (std::uint32_t i = 0; i < n; ++i) {
      float v;
      std::memcpy(&v, params + 4u * i, 4);
      if (v == v && v > -1.0e30f && v < 1.0e30f) {
        in.mod->set_param(in.state, i, v);
        (*blobs_)[idx][i] = v;
        in.set_mask |= 1u << i;
      }
    }
    leave(prev);
  }
}

// The right record of the master's pair follows the left one's main-loop flags: the pair has ONE set of commands (they name the left record; the render applies them to both).
void InsertHost::sync_pair(Inst& lead) noexcept {
  Inst* r = mate_of(lead);
  if (r == nullptr || lead.where != kMasterWhere) {
    return;
  }
  r->ml = lead.ml;
  r->act_pushed = lead.act_pushed;
  r->deact_pushed = lead.deact_pushed;
  r->byp_want = lead.byp_want;
  r->byp_pushed = lead.byp_pushed;
  r->then_reactivate = lead.then_reactivate;
}

InsertHost::Bind InsertHost::bind(unsigned where, const o3m_module& mod, std::uint16_t fault_id, void* state, const std::uint8_t* params, std::uint32_t param_len,
                                  bool force, void* state_r) noexcept {
  if (graph_ == nullptr || voices_ == nullptr || blobs_ == nullptr) {
    return Bind::NoGraph;
  }
  if (where >= kWhere || where == kMasterWhere + 1u) {
    return Bind::BadWhere;
  }
  const unsigned copies = instances_for(where, mod);  // 2 for the master's dual mono, 1 for the master's stereo (A7) or a track
  if (wh_[where] != kNone || (copies == 2u && wh_[where + 1u] != kNone)) {
    return Bind::WhereTaken;
  }
  if (mod.kind != O3M_KIND_EFFECT || mod.render == nullptr || mod.init == nullptr || mod.shutdown == nullptr) {
    return Bind::BadModule;
  }
  Inst* pick[2] = {nullptr, nullptr};
  void* const given[2] = {state, state_r};
  if (state == nullptr) {
    for (unsigned k = 0; k < copies; ++k) {
      for (unsigned i = 0; i < kInstances && pick[k] == nullptr; ++i) {  // the free list: a record that held this module keeps its state block
        if (in_[i].ml == Ml::Free && in_[i].mod == &mod && in_[i].state != nullptr && &in_[i] != pick[0]) {
          pick[k] = &in_[i];
        }
      }
    }
    if (pick[0] == nullptr || (copies == 2u && pick[1] == nullptr)) {
      unsigned free_records = 0;
      for (unsigned i = 0; i < kInstances; ++i) {
        free_records += in_[i].ml == Ml::Free ? 1u : 0u;
      }
      return free_records >= copies ? Bind::NeedState : Bind::NoInstance;
    }
  } else {
    if ((reinterpret_cast<std::uintptr_t>(state) & 15u) != 0u || (copies == 2u && (state_r == nullptr || (reinterpret_cast<std::uintptr_t>(state_r) & 15u) != 0u))) {
      return Bind::BadState;
    }
    for (unsigned k = 0; k < copies; ++k) {
      for (unsigned i = 0; i < kInstances && pick[k] == nullptr; ++i) {  // a never used record first, then one of another module (its state block stays lost until the arena is reset)
        pick[k] = (in_[i].ml == Ml::Free && in_[i].mod == nullptr && &in_[i] != pick[0]) ? &in_[i] : nullptr;
      }
      for (unsigned i = 0; i < kInstances && pick[k] == nullptr; ++i) {
        pick[k] = (in_[i].ml == Ml::Free && &in_[i] != pick[0]) ? &in_[i] : nullptr;
      }
      if (pick[k] == nullptr) {
        return Bind::NoInstance;
      }
    }
  }
  const bool reuse = state == nullptr;
  std::uint32_t known = 0;
  for (unsigned k = 0; k < copies && reuse; ++k) {
    known += pick[k]->est.load(std::memory_order_relaxed);
  }
  if (!force && !admit(known, true, copies)) {
    return Bind::NoCpu;
  }
  unsigned inited = 0;
  for (unsigned k = 0; k < copies; ++k) {
    Inst& p = *pick[k];
    if (!reuse) {
      p.mod = &mod;
      p.state = given[k];
      p.est.store(0, std::memory_order_relaxed);
    }
    p.fault_id = fault_id;
    p.set_mask = 0;
    if (!init_module(p)) {
      for (unsigned j = 0; j < inited; ++j) {  // the first instance came up: take it down again (it never reached the render)
        shutdown_module(*pick[j]);
      }
      for (unsigned j = 0; j <= k && !reuse; ++j) {
        pick[j]->mod = nullptr;
        pick[j]->state = nullptr;
      }
      return Bind::InitFailed;
    }
    ++inited;
  }
  for (unsigned k = 0; k < copies; ++k) {
    start_binding(*pick[k], where + k, params, param_len);
    pick[k]->mate = copies == 2u ? static_cast<std::uint8_t>(pick[1u - k] - in_) : kNone;
  }
  const Command c{Cmd::Activate, static_cast<std::uint8_t>(pick[0] - in_), static_cast<std::uint8_t>(where >> 1), static_cast<std::uint8_t>(where & 1u), pick[0]->gen, 0, 0};
  pick[0]->act_pushed = push(c);
  sync_pair(*pick[0]);
  return Bind::Ok;
}

bool InsertHost::unbind(unsigned where) noexcept {
  if (where >= kWhere || where == kMasterWhere + 1u || wh_[where] == kNone || in_[wh_[where]].ml != Ml::Bound) {
    return false;
  }
  Inst& in = in_[wh_[where]];
  in.ml = Ml::Leaving;
  in.then_reactivate = false;
  sync_pair(in);
  service();
  return true;
}

InsertHost::Toggle InsertHost::set_bypass(unsigned where, bool on, bool force) noexcept {
  if (where >= kWhere || where == kMasterWhere + 1u || wh_[where] == kNone || in_[wh_[where]].ml != Ml::Bound) {
    return Toggle::NotBound;
  }
  Inst& in = in_[wh_[where]];
  if (on) {
    in.byp_want = true;
    sync_pair(in);
    service();
    return Toggle::Done;
  }
  if (!in.byp_want && in.forced.load(std::memory_order_relaxed) == 0u) {
    return Toggle::Done;  // not bypassed
  }
  const Inst* m = mate_of(in);
  const std::uint32_t known = in.est.load(std::memory_order_relaxed) + (m != nullptr ? m->est.load(std::memory_order_relaxed) : 0u);
  if (!force && !admit(known, false, m != nullptr ? 2u : 1u)) {
    return Toggle::NoCpu;
  }
  in.ml = Ml::Leaving;  // a reset: the state is the module's fresh one, through the swap protocol
  in.then_reactivate = true;
  sync_pair(in);
  service();
  return Toggle::Done;
}

// The master's parameters go to both of the dual-mono pair's instances (the same values); a stereo master (A7: one instance) and a track's get theirs once.
bool InsertHost::set_param(unsigned where, unsigned id, float value) noexcept {
  if (where == kMasterWhere + 1u) {
    return false;  // the right instance is the pair's, never addressed alone
  }
  const bool first = set_param_at(where, id, value);
  if (where != kMasterWhere || wh_[where + 1u] == kNone) {  // A7: a stereo master is one record: wh_[33] is free
    return first;
  }
  return set_param_at(where + 1u, id, value) && first;
}

bool InsertHost::set_param_at(unsigned where, unsigned id, float value) noexcept {
  if (where >= kWhere || wh_[where] == kNone || value != value) {
    return false;
  }
  Inst& in = in_[wh_[where]];
  const std::uint32_t cap = in.mod->param_count < kParams ? in.mod->param_count : kParams;
  if (id >= cap) {
    return false;
  }
  const unsigned idx = static_cast<unsigned>(&in - in_);
  (*blobs_)[idx][id] = value;
  in.set_mask |= 1u << id;
  if (in.ml != Ml::Bound) {
    return true;  // leaving: the copy is what a reset applies
  }
  if (!in.act_pushed) {
    if (in.mod->set_param != nullptr) {  // the render does not hold the binding yet: the module is still the main loop's
      const std::uint16_t prev = enter(in);
      in.mod->set_param(in.state, id, value);
      leave(prev);
    }
    return true;
  }
  if (where == kMasterWhere + 1u) {
    return true;  // the pair's right instance: the left one's ParamChange (key 32) reaches both at the block start (apply_param), nothing is sent for it
  }
  in.dirty |= 1u << id;
  flush_params(in);
  return (in.dirty & (1u << id)) == 0u || !live(in);  // sent, or waiting for the render to take the binding (service() sends it)
}

// The dirty parameters of a live binding into the graph's ring; what does not fit stays dirty for the next tick.
void InsertHost::flush_params(Inst& in) noexcept {
  if (in.dirty == 0u || !live(in)) {
    return;
  }
  const unsigned idx = static_cast<unsigned>(&in - in_);
  for (unsigned id = 0; id < kParams; ++id) {
    if (((in.dirty >> id) & 1u) == 0u) {
      continue;
    }
    const core::ParamChange c{core::param_id(core::ParamKind::Insert, in.where, id), (*blobs_)[idx][id], in.gen};
    if (!graph_->params().push(c)) {
      return;
    }
    in.dirty &= ~(1u << id);
  }
}

void InsertHost::clear_where(Inst& in) noexcept {
  if (in.where < kWhere && wh_[in.where] == static_cast<std::uint8_t>(&in - in_)) {
    wh_[in.where] = kNone;
  }
  in.where = kNone;
}

// `in` is a lone record or the left record of the master's pair (the pair leaves, and is reset, together).
void InsertHost::finish_leave(Inst& in) noexcept {
  const unsigned where = in.where;
  Inst* m = mate_of(in);
  const bool again = in.then_reactivate;
  if (!again) {
    shutdown_module(in);
    if (m != nullptr) {
      shutdown_module(*m);
    }
  }
  if (!again || !restart_module(in) || (m != nullptr && !restart_module(*m))) {
    shutdown_module(in);  // (a no-op for a record that is shut already: the other one of the pair may have come up)
    clear_where(in);
    in.ml = Ml::Free;  // the record keeps its module and state block: the next bind of the same module reuses it
    in.mate = kNone;
    if (m != nullptr) {
      shutdown_module(*m);
      clear_where(*m);
      m->ml = Ml::Free;
      m->mate = kNone;
    }
    return;
  }
  start_binding(in, where, nullptr, 0);  // (set_mask is kept: no params; the stored values were applied by restart_module, which start_binding's dirty = 0 follows)
  if (m != nullptr) {
    start_binding(*m, where + 1u, nullptr, 0);
    sync_pair(in);
  }
}

void InsertHost::service() noexcept {
  if (graph_ == nullptr) {
    return;
  }
  for (unsigned i = 0; i < kInstances; ++i) {
    Inst& in = in_[i];
    if (in.where == kMasterWhere + 1u) {
      continue;  // S6: the master's right record has no commands of its own (the left one's apply to both)
    }
    Inst* m = mate_of(in);
    if (in.ml == Ml::Bound) {
      if (!in.act_pushed) {
        const Command c{Cmd::Activate, static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(in.where >> 1), static_cast<std::uint8_t>(in.where & 1u), in.gen, 0, 0};
        in.act_pushed = push(c);
        sync_pair(in);
        continue;
      }
      if (in.byp_want && !in.byp_pushed) {
        const Command c{Cmd::Bypass, static_cast<std::uint8_t>(i), 0, 0, in.gen, 1, 0};
        in.byp_pushed = push(c);
        sync_pair(in);
      }
      flush_params(in);
      if (m != nullptr) {
        flush_params(*m);
      }
    } else if (in.ml == Ml::Leaving) {
      bool done = !in.act_pushed;  // the render never saw this binding
      if (!done) {
        if (!in.deact_pushed) {
          const Command c{Cmd::Deactivate, static_cast<std::uint8_t>(i), 0, 0, in.gen, 0, 0};
          in.deact_pushed = push(c);
          sync_pair(in);
        }
        done = in.deact_pushed && retired_ok(in) && (m == nullptr || retired_ok(*m));
      }
      if (done) {
        finish_leave(in);
      }
    }
  }
}

void InsertHost::retire_all() noexcept {
  for (unsigned i = 0; i < kInstances; ++i) {
    if (in_[i].ml == Ml::Bound) {
      in_[i].ml = Ml::Leaving;
      in_[i].then_reactivate = false;
      sync_pair(in_[i]);
    }
  }
  service();
}

bool InsertHost::settled() const noexcept {
  for (unsigned i = 0; i < kInstances; ++i) {
    const Inst& in = in_[i];
    if (in.ml == Ml::Bound) {
      return false;
    }
    if (in.ml == Ml::Leaving && in.act_pushed && !(in.deact_pushed && retired_ok(in))) {
      return false;
    }
  }
  return true;
}

void InsertHost::release_all() noexcept {
  for (unsigned i = 0; i < kInstances; ++i) {
    Inst& in = in_[i];
    if (graph_ != nullptr && in.phase != Phase::Off) {  // only a forced release gets here with the render holding it (settled() is the rule): the slot must not outlive the module. After the clear the render may still be inside the slot's fn (it tested fn before this write): the same known hazard as slot_clear — one block at most, and the state block is not reused before the arena reset
      slot_clear(in);
      clear_tail(in);
      rwh_[in.track * 2u + in.slot] = kNone;
      in.phase = Phase::Off;
      in.pub_phase.store(0, std::memory_order_relaxed);
      in.pub_gen.store(0, std::memory_order_relaxed);
    }
    shutdown_module(in);
    in.mod = nullptr;
    in.state = nullptr;
    in.ml = Ml::Free;
    in.where = kNone;
    in.mate = kNone;
    in.set_mask = 0;
    in.dirty = 0;
    in.est.store(0, std::memory_order_relaxed);
  }
  for (unsigned w = 0; w < kWhere; ++w) {
    wh_[w] = kNone;
  }
  if (graph_ != nullptr) {  // OS-INSFX-ABI2 A6: no key tap outlives the modules, no key mask the project it came from
    for (unsigned t = 0; t < kTracks; ++t) {
      if (((tap_mask_ >> t) & 1u) != 0u) {
        graph_->clear_insert(t, 1u);
      }
    }
  }
  tap_mask_ = 0;
  key_prev_ = 0;
  module::g_keys.clear();
}

bool InsertHost::prepare_export() noexcept {
  if (graph_ == nullptr) {
    return true;
  }
  // The render is parked: this is the only context. What was queued is applied and every transition finished as the first blocks would, bounded.
  bool settled_ok = false;
  for (unsigned pass = 0; pass < kExportPasses; ++pass) {
    for (unsigned i = 0; i < kInstances; ++i) {
      if (in_[i].phase == Phase::In) {
        in_[i].mix = in_[i].target;  // the first export block is fully wet: the fade-in belongs to the live render
      }
    }
    begin_block();
    service();
    bool stable = ring_.empty();
    for (unsigned i = 0; i < kInstances && stable; ++i) {
      const Inst& in = in_[i];
      stable = (in.phase == Phase::Off || in.phase == Phase::On || in.phase == Phase::Bypassed) && in.ml != Ml::Leaving && (in.ml != Ml::Bound || in.act_pushed);
    }
    if (stable) {
      settled_ok = true;
      break;
    }
  }
  if (!settled_ok) {  // never fall through with a transition half done: the export is refused, the live render goes on with the nets on
    ++stats_.export_unsettled;
    export_end();
    return false;
  }
  bool restart_ok = true;
  for (unsigned i = 0; i < kInstances; ++i) {  // every running effect starts the export from its fresh state
    Inst& in = in_[i];
    if (in.ml == Ml::Bound && in.phase == Phase::On && in.rgen == in.gen) {
      if (!restart_module(in)) {  // its init failed: the state is zeroed and shut, so the live render must not run it again; it leaves (as an unbind), and the export is refused
        in.ml = Ml::Leaving;
        in.then_reactivate = false;
        restart_ok = false;
        if (Inst* m = mate_of(in)) {  // S6: the master's pair goes together (one channel of a dual-mono effect is not a sound)
          m->ml = Ml::Leaving;
          m->then_reactivate = false;
        }
      }
      in.idle = false;
      clear_tail(in);  // an export starts with no tail (the render is parked: the Graph's byte is written from here)
      in.prev = 0;
      in.over = 0;
      in.acc = 0;
    }
  }
  if (!restart_ok) {
    ++stats_.export_unsettled;
    export_end();
    service();  // the Deactivate of what failed goes to the ring; the live render applies it at its next boundary
    return false;
  }
  agg_over_ = 0;
  nets_ = false;
  module::g_keys.clear_words();  // OS-INSFX-ABI2 A6: two exports of a song give the same bits: no key level of the live render is read in the export's first block
  return true;
}

void InsertHost::export_end() noexcept {
  nets_ = true;
  agg_over_ = 0;
  // (the means of the render's other work are kept: they are not followed while the nets are off, and a reseed from one cold block could force an insert that was on)
  for (unsigned i = 0; i < kInstances; ++i) {
    in_[i].prev = 0;
    in_[i].over = 0;
    in_[i].acc = 0;
  }
}

bool InsertHost::info(unsigned i, Info& out) const noexcept {
  if (i >= kInstances || in_[i].ml == Ml::Free) {
    return false;
  }
  const Inst& in = in_[i];
  out.where = in.where;
  out.mod = in.mod;
  out.state = in.state;
  out.phase = static_cast<Phase>(in.pub_phase.load(std::memory_order_relaxed));
  out.user_bypass = in.byp_want;
  out.forced = in.forced.load(std::memory_order_relaxed) != 0u;
  out.leaving = in.ml == Ml::Leaving;
  out.tail = in.tail.load(std::memory_order_relaxed);
  out.gen = in.gen;
  out.cost = in.est.load(std::memory_order_relaxed);
  return true;
}

std::uint32_t InsertHost::cost(unsigned where) const noexcept {
  if (where >= kWhere || wh_[where] == kNone || where == kMasterWhere + 1u) {
    return 0u;
  }
  const Inst& in = in_[wh_[where]];
  const std::uint32_t own = in.est.load(std::memory_order_relaxed);
  return in.mate != kNone ? own + in_[in.mate].est.load(std::memory_order_relaxed) : own;  // the master's is both instances'
}

// ---- render context -------------------------------------------------------------------------------------------------------------------------

// A record's place in the graph. A track's insert is a slot of the strip (and a wet share the stereo side follows, and a tail that keeps the strip live); the master's pair is one stage of the master chain
// (S6: the planes are float and stereo already, and the chain runs every block while the stage is not bypassed, so neither exists there; the stage is the pair's, so a bypass or a clear from either record is
// the same store).
void InsertHost::slot_wet(const Inst& in, unsigned wet0, unsigned wet1) noexcept {
  if (in.track < kTracks) {
    graph_->set_insert_wet(in.track, in.slot, wet0, wet1);
  }
}

void InsertHost::slot_bypass(const Inst& in, bool bypass) noexcept {
  if (in.track < kTracks) {
    graph_->set_insert_bypass(in.track, in.slot, bypass);
    if (in.slot == 1u) {
      tap_mask_ = static_cast<std::uint16_t>(tap_mask_ & ~(1u << in.track));  // (OS-INSFX-ABI2 A6: the slot holds the module; key_boundary() puts the tap over a bypassed one on a keyed track)
    }
  } else {
    graph_->set_master_bypass(MasterStage::Insert, bypass);
  }
}

void InsertHost::slot_clear(const Inst& in) noexcept {
  if (in.track < kTracks) {
    graph_->clear_insert(in.track, in.slot);
    if (in.slot == 1u) {
      tap_mask_ = static_cast<std::uint16_t>(tap_mask_ & ~(1u << in.track));  // (OS-INSFX-ABI2 A6: an empty slot B has no tap until key_boundary() puts it back)
    }
  } else {
    graph_->install_master_stage(MasterStage::Insert, nullptr, nullptr);
  }
}

// The wet share goes from where it is to dry or to wet over the coming block; the Graph is told for the stereo side.
void InsertHost::leave_ramp(Inst& in, Phase to) noexcept {
  slot_wet(in, wet_of(in.mix), 0u);
  in.target = 0.0f;
  in.phase = to;
  in.pub_phase.store(static_cast<std::uint8_t>(to), std::memory_order_relaxed);
}

void InsertHost::clear_tail(Inst& in) noexcept {
  in.tail.store(false, std::memory_order_relaxed);
  in.zero = 0;
  in.age = 0;
  if (in.track < kTracks) {
    graph_->set_insert_tail(in.track, in.slot, false);
  }
}

// A net's forced bypass takes the master's pair together (a dual-mono effect on one channel only would be a different sound).
void InsertHost::force_bypass(Inst& in) noexcept {
  in.forced.store(1, std::memory_order_relaxed);
  leave_ramp(in, Phase::OutBypass);
  if (Inst* m = mate_of(in)) {
    if (m->phase == Phase::In || m->phase == Phase::On) {
      m->forced.store(1, std::memory_order_relaxed);
      leave_ramp(*m, Phase::OutBypass);
    }
  }
}

// A binding the render takes: the record's part of the Activate (the pair's two records are done together).
void InsertHost::activate(Inst& in, unsigned track, unsigned slot, std::uint16_t gen) noexcept {
  in.rgen = gen;
  in.track = static_cast<std::uint8_t>(track);
  in.slot = static_cast<std::uint8_t>(slot);
  in.shape = static_cast<std::uint8_t>(shape_of(*in.mod));  // OS-INSFX-ABI2 A4: the shape is the module's, as on a track (A7: the master's stereo instance renders render_st from it, in run_master)
  in.st_on = false;
  rwh_[track * 2u + slot] = static_cast<std::uint8_t>(&in - in_);
  in.idle = false;
  in.mix = 0.0f;
  in.target = 1.0f;
  in.acc = 0;
  in.prev = 0;
  in.over = 0;
  in.est_at = blocks_.load(std::memory_order_relaxed);
  in.phase = Phase::In;  // (the estimate is kept across a bypass off or a re-bind)
  in.pub_gen.store(gen, std::memory_order_release);
  in.pub_phase.store(static_cast<std::uint8_t>(Phase::In), std::memory_order_relaxed);
}

void InsertHost::apply(const Command& c) noexcept {
  if (c.inst >= kInstances) {
    return;
  }
  Inst& in = in_[c.inst];
  if (c.gen != in.cur_gen.load(std::memory_order_relaxed)) {  // queued for a binding that has since been released (release_all with no render) and whose record was bound again
    return;
  }
  Inst* const m = in.mate != kNone ? &in_[in.mate] : nullptr;  // S6: the master's pair: the commands name the left record and act on both
  switch (c.kind) {
    case Cmd::Activate: {
      if (in.phase != Phase::Off || c.track > kTracks || c.slot > 1u || in.mod == nullptr) {
        return;
      }
      const bool master = c.track == kTracks;
      // A7: a stereo module on the master is ONE record (mate kNone); a mono one is the S6 pair and needs its mate.
      const bool st_master = master && in.mate == kNone && shape_of(*in.mod) != Shape::Mono;
      if (master && (c.slot != 0u || (!st_master && (m == nullptr || m->phase != Phase::Off || m->mod == nullptr)) || rwh_[kMasterWhere + 1u] != kNone)) {
        return;
      }
      const unsigned where = c.track * 2u + c.slot;
      if (rwh_[where] != kNone) {
        return;
      }
      activate(in, c.track, c.slot, c.gen);
      if (master) {
        if (m != nullptr) {
          activate(*m, c.track, 1u, m->cur_gen.load(std::memory_order_relaxed));
        }
        graph_->install_master_stage(MasterStage::Insert, &InsertHost::run_master, &in);
        graph_->set_master_bypass(MasterStage::Insert, false);
        if (m != nullptr) {
          clear_tail(*m);
        }
      } else {
        graph_->install_insert(c.track, c.slot, c.slot == 1u ? &InsertHost::run_keyed : &InsertHost::run, &in);  // (OS-INSFX-ABI2 A6: slot B measures the key of its track while it is one)
        graph_->set_insert_bypass(c.track, c.slot, false);
        graph_->set_insert_wet(c.track, c.slot, 0u, 255u);
        if (c.slot == 1u) {
          tap_mask_ = static_cast<std::uint16_t>(tap_mask_ & ~(1u << c.track));  // the module takes the slot over a tap
        }
      }
      read_keys(in);  // OS-INSFX-ABI2 A6: the key tracks of the binding (the master's pair: both records; A7's stereo master: its one)
      if (master && m != nullptr) {
        read_keys(*m);
      }
      clear_tail(in);
      ++stats_.activations;
      break;
    }
    case Cmd::Deactivate:
      if (in.phase != Phase::Off && in.phase != Phase::OutLeave && in.rgen == c.gen) {
        leave_ramp(in, Phase::OutLeave);
        if (m != nullptr && m->phase != Phase::Off && m->phase != Phase::OutLeave) {
          leave_ramp(*m, Phase::OutLeave);
        }
      }
      break;
    case Cmd::Bypass:
      if (c.on != 0u && (in.phase == Phase::In || in.phase == Phase::On) && in.rgen == c.gen) {
        leave_ramp(in, Phase::OutBypass);
        if (m != nullptr && (m->phase == Phase::In || m->phase == Phase::On)) {
          leave_ramp(*m, Phase::OutBypass);
        }
      }
      break;
  }
}

bool InsertHost::apply_param(const core::ParamChange& c) noexcept {
  const unsigned where = core::param_index(c.id);
  const unsigned field = core::param_field(c.id);
  const std::uint8_t idx = where < kWhere && where != kMasterWhere + 1u ? rwh_[where] : kNone;
  if (idx == kNone) {
    ++stats_.stale;
    return false;
  }
  Inst& in = in_[idx];
  if (in.phase == Phase::Off || in.phase == Phase::OutLeave || in.rgen != static_cast<std::uint16_t>(c.ramp_samples & 0xFFFFu)) {
    ++stats_.stale;  // not bound, leaving, or queued for a binding that has since left (and whose record may have been reused)
    return false;
  }
  if (field >= in.mod->param_count || field >= kParams || in.mod->set_param == nullptr) {
    return false;
  }
  const std::uint16_t prev = enter(in);
  in.mod->set_param(in.state, field, c.value);
  leave(prev);
  ++stats_.params;
  set_key(in, field, c.value);  // OS-INSFX-ABI2 A6: a KEY parameter moves the instance's key track
  if (where == kMasterWhere && in.mate != kNone) {  // S6: the master's key reaches both instances (dual mono: the same value; the left one's generation is the pair's)
    Inst& r = in_[in.mate];
    if (r.phase != Phase::Off && r.phase != Phase::OutLeave) {
      const std::uint16_t p2 = enter(r);
      r.mod->set_param(r.state, field, c.value);
      leave(p2);
      ++stats_.params;
      set_key(r, field, c.value);
    }
  }
  return true;
}

// The estimate and the nets, for the block that just ended.
void InsertHost::account() noexcept {
  if (!nets_) {
    for (unsigned i = 0; i < kInstances; ++i) {
      in_[i].acc = 0;
    }
    return;
  }
  const gov::Governor& g = voices_->governor();
  const std::uint32_t now = blocks_.load(std::memory_order_relaxed);
  const std::uint32_t budget = g.budget();
  std::uint32_t total = 0;
  bool any = false;
  Inst* worst = nullptr;
  std::uint32_t worst_cost = 0;
  for (unsigned i = 0; i < kInstances; ++i) {
    Inst& in = in_[i];
    const std::uint32_t sample = in.acc;
    in.acc = 0;
    if (in.phase == Phase::Off || in.phase == Phase::Bypassed) {
      continue;  // a bypassed insert is not run, so not measured
    }
    std::uint32_t est = in.est.load(std::memory_order_relaxed);
    if (sample != 0u) {
      any = true;
      total += sample;
      const std::uint32_t prev = in.prev;  // as the governor does it: min(this block, the previous one), a moving max with decay
      in.prev = sample;
      const std::uint32_t confirmed = prev == 0u ? 0u : (prev < sample ? prev : sample);
      if (confirmed != 0u) {
        const std::uint32_t decayed = gov::decayed_cost(est, now - in.est_at);
        est = confirmed > decayed ? confirmed : decayed;
        in.est.store(est, std::memory_order_relaxed);
        in.est_at = now;
      }
      if (sample <= budget) {
        in.over = 0;
      } else if (in.over < 255u) {
        ++in.over;
      }
      if (in.over >= gov::kMuteBlocks && (in.phase == Phase::In || in.phase == Phase::On)) {
        in.over = 0;
        ++stats_.forced_instance;
        force_bypass(in);
        continue;
      }
    }
    if ((in.phase == Phase::In || in.phase == Phase::On) && !(in.track == kTracks && in.slot == 1u)) {  // (the master's pair is one candidate: the left record stands for both, at twice the cost)
      std::uint32_t c = est != 0u ? est : sample;
      c = in.mate != kNone ? 2u * c : c;  // the pair costs what its two instances do (the same module, the same block)
      if (c >= worst_cost) {
        worst = &in;
        worst_cost = c;
      }
    }
  }
  // The rest of the render, this block (what the governor saw less the inserts' own cycles, measured here), as two moving means (the file's constants): a first block seeds them, at most the threshold, and the
  // next blocks average in at once (a cold block at boot must not hold an insert that was on over the threshold for long). The base is a moving MAX, which on a busy project that plays without underruns (mean 86 %, peaks past 100 %) sits over the threshold, so an allowance taken from it
  // was 0 and any insert was forced (OS-INSFX S2 unit regression); a single slow mean followed a light render that turned heavy only after 110 blocks.
  const std::uint32_t threshold = g.threshold();
  const std::uint32_t others_now = g.last_total() > total ? g.last_total() - total : 0u;  // all the render's work but the inserts: the module voices count
  if (others_n_ == 0u) {
    others_slow_ = others_fast_ = others_now < threshold ? others_now : threshold;
    others_n_ = 1;
  } else {
    if (others_n_ < kOthersSlowBlocks) {
      ++others_n_;
    }
    others_slow_ = running_mean(others_slow_, others_now, others_n_, kOthersSlowBlocks);
    others_fast_ = running_mean(others_fast_, others_now, others_n_, kOthersFastBlocks);
  }
  const std::uint32_t deadband = gov::cycles_of_percent(kOthersDeadbandPercent);
  const std::uint32_t others = others_fast_ > others_slow_ + deadband ? others_fast_ - deadband : others_slow_;
  others_pub_.store(others, std::memory_order_relaxed);
  if (!any) {
    agg_over_ = 0;
    return;
  }
  total_pub_.store(total, std::memory_order_relaxed);
  // The aggregate net: the inserts together against what the threshold leaves once the rest of the render (`others`, without the inserts) is taken off, and never above the instance budget (so all the inserts
  // together take at most the budget: spec 6). An insert is forced only when the inserts push the render over the threshold for N blocks.
  const std::uint32_t room = threshold > others ? threshold - others : 0u;
  const std::uint32_t agg = room < budget ? room : budget;
  if (total <= agg) {
    agg_over_ = 0;
    return;
  }
  if (++agg_over_ >= gov::kMuteBlocks && worst != nullptr && (worst->phase == Phase::In || worst->phase == Phase::On)) {
    agg_over_ = 0;  // the next N blocks judge what is left
    ++stats_.forced_aggregate;
    force_bypass(*worst);
  }
}

void InsertHost::begin_block() noexcept {
  if (graph_ == nullptr) {
    return;
  }
  if (__builtin_expect(s_used_ != 0u, 0)) {
    clear_sides();  // OS-INSFX-ABI2 A4, step 8: the S the stereo inserts handed on lived for the block that ended
  }
  const std::uint32_t now = blocks_.load(std::memory_order_relaxed) + 1u;
  blocks_.store(now, std::memory_order_release);
  // 1. The transitions of the block that just ran end here, whether or not run() was reached (an idle strip never calls it again): a slot that was ramping out is retired.
  for (unsigned i = 0; i < kInstances; ++i) {
    Inst& in = in_[i];
    switch (in.phase) {
      case Phase::In:
        if (in.mix == in.target) {  // the ramp ran (a strip that was idle has not heard the effect yet: it fades in when it first runs)
          slot_wet(in, 255u, 255u);
          in.phase = Phase::On;
          in.pub_phase.store(static_cast<std::uint8_t>(Phase::On), std::memory_order_relaxed);
        }
        break;
      case Phase::OutBypass:
        slot_bypass(in, true);
        slot_wet(in, 255u, 255u);
        clear_tail(in);
        in.mix = 0.0f;
        in.phase = Phase::Bypassed;
        in.pub_phase.store(static_cast<std::uint8_t>(Phase::Bypassed), std::memory_order_relaxed);
        break;
      case Phase::OutLeave:
        slot_clear(in);
        clear_tail(in);
        rwh_[in.track * 2u + in.slot] = kNone;
        in.phase = Phase::Off;
        in.mix = 0.0f;
        ++stats_.retired;
        in.retired_blk.store(now, std::memory_order_relaxed);
        in.retired_gen.store(in.rgen, std::memory_order_release);
        in.pub_gen.store(0, std::memory_order_relaxed);
        in.pub_phase.store(0, std::memory_order_relaxed);
        break;
      case Phase::On:
      case Phase::Bypassed:
      case Phase::Off:
        break;
    }
  }
  // OS-INSFX-ABI2 A5, the cap (spec 3.2): the WIDENS track inserts that run (In, On and the two ramps out; not Bypassed or Off) keep a side block each from the stereo voices. The count is of inserts, not tracks
  // (two on one track cost two). The master reserves none: A7 makes its stereo insert one instance with mate == kNone, so the count tests `where`, not the mate (kMasterWhere, and the S6 pair's right record).
  if (pool_ != nullptr) {
    unsigned wide = 0;
    for (unsigned i = 0; i < kInstances; ++i) {
      const Inst& in = in_[i];
      if (in.where != kMasterWhere && in.where != kMasterWhere + 1u && in.shape == static_cast<std::uint8_t>(Shape::Widens) &&
          (in.phase == Phase::In || in.phase == Phase::On || in.phase == Phase::OutBypass || in.phase == Phase::OutLeave)) {
        ++wide;
      }
    }
    if (wide != st_res_) {
      st_res_ = static_cast<std::uint8_t>(wide);
      pool_->set_st_reserved(wide);
    }
  }
  // 2. The block's measures, the nets.
  account();
  // 3. What the main loop sent since the last boundary.
  Command c;
  for (std::uint32_t n = 0; n < kMaxCommandsPerBlock && ring_.pop(c); ++n) {
    apply(c);
  }
  // 4. OS-INSFX-ABI2 A6: the key words age, the key mask and the taps follow the instances as they are now.
  key_boundary();
}

void InsertHost::clear_sides() noexcept {
  for (std::uint32_t m = s_used_; m != 0u; m &= m - 1u) {
    const unsigned t = static_cast<unsigned>(__builtin_ctz(m));
    if (graph_->side(t) == sblk_) {  // (a later insert of the strip may have handed on null, or Engine::feed_sides a voice's block: those are not ours)
      graph_->feed_side(t, nullptr);
    }
  }
  s_used_ = 0;
}

// The Graph's InsertFn: in place on the track's mono Q15 block. A stereo effect decides here, once a block, whether it runs stereo (the input has S, or it WIDENS) and tells the Graph's side_pass when that changes.
void InsertHost::run(void* state, std::int16_t* io, std::uint32_t n) noexcept {
  Inst& in = *static_cast<Inst*>(state);
  if (n == 0u || n > kBlock) {
    return;
  }
  if (in.shape == static_cast<std::uint8_t>(Shape::Mono)) {
    process(in, io, n, false, nullptr, 0u, 0u);
    return;
  }
  Graph& g = *in.host->graph_;
  std::uint32_t g0;
  std::uint32_t g1;
  g.side_gain(in.track, in.slot, g0, g1);  // the share of the voice's S the mono inserts before this one leave (step 1); 0 at both ends: no S
  const std::int16_t* const sin = (g0 | g1) != 0u ? g.side(in.track) : nullptr;
  const bool dry = in.mix == 0.0f && in.target == 0.0f;  // a ramp that left before it ever ran: the effect is not called, S goes on as the mono rule has it
  // A stereo-capable effect that ran stereo and has a tail stays stereo with a zero S input while its source is silent: its S part rings on, never cut to the mono render in one block (a reverb on a track whose stereo
  // sample ended). A live mono source (M not silent, no S) is the mono path, as for any capable effect.
  const bool stereo = !dry && (in.shape == static_cast<std::uint8_t>(Shape::Widens) || sin != nullptr || (in.st_on && in.tail.load(std::memory_order_relaxed) && block_silent(io, n)));
  if (stereo != in.st_on) {  // before every return of process(): an idle effect must not leave a stale mark either
    in.st_on = stereo;
    g.set_insert_stereo(in.track, in.slot, stereo);
  }
  process(in, io, n, stereo, sin, g0, g1);
}

void InsertHost::process(Inst& in, std::int16_t* io, std::uint32_t n, bool stereo, const std::int16_t* sin, std::uint32_t g0, std::uint32_t g1) noexcept {
  InsertHost& h = *in.host;
  const std::uint32_t t0 = h.cycles_();
  const bool silent_in = block_silent(io, n) && (sin == nullptr || block_silent(sin, n));  // (a live source ends the scan at its first word; a stereo effect needs M and S both silent)
  if (in.idle && silent_in) {  // the effect said silent and idle and has nothing to add to silence
    in.mix = in.target;
    in.acc += h.cycles_() - t0;
    return;
  }
  if (in.mix == 0.0f && in.target == 0.0f) {  // a ramp that left before it ever ran (or a bypass / unbind in the block of its Activate): dry from the first sample, the effect is not called
    return;
  }
  const bool ramp = in.mix != in.target;
  std::uint32_t r;
  bool s_zero = true;  // S' of a mono effect: none
  if (stereo) {
    r = h.stereo_block(in, io, n, sin, g0, g1, ramp, s_zero);
  } else {
    float* const fl = h.graph_->insert_scratch(0);
    kernel::q15_to_float(fl, io, n);
    float* dry = nullptr;
    if (ramp) {
      dry = h.graph_->insert_scratch(1);
      for (std::uint32_t i = 0; i < n; ++i) {
        dry[i] = fl[i];
      }
    }
    const std::uint16_t prev = h.enter(in);
    r = in.mod->render(in.state, fl, fl, n);
    h.leave(prev);
    in.idle = r == 0u;
    if (ramp) {  // one block, linear: the dry signal and the effect's, never a step
      float m = in.mix;
      const float dm = (in.target - m) / static_cast<float>(n);
      for (std::uint32_t i = 0; i < n; ++i) {
        m += dm;
        fl[i] = dry[i] + (fl[i] - dry[i]) * m;
      }
      in.mix = in.target;
    }
    h.stats_.saturated += to_q15(io, fl, n);
  }
  // S2, the tail: the effect rings on while it returns 1, unless its output has been exactly zero for kTailZeroBlocks blocks (2 s) with the source silent, or its input for 10 s (a module stuck at 1). A tail that ends also marks the
  // effect idle, so a silent input skips it (as after a 0) until the input is not silent again. A stereo effect's output is M' and S', both zero.
  bool tail = false;
  if (r != 0u) {
    in.age = silent_in ? static_cast<std::uint16_t>(in.age < kTailMaxBlocks ? in.age + 1u : kTailMaxBlocks) : 0u;
    in.zero = (silent_in && s_zero && block_silent(io, n)) ? static_cast<std::uint16_t>(in.zero < kTailZeroBlocks ? in.zero + 1u : kTailZeroBlocks) : 0u;  // (a live source resets it: a pre-delay's zeros while playing do not count)
    tail = in.zero < kTailZeroBlocks && in.age < kTailMaxBlocks;
    in.idle = !tail && silent_in;
  } else {
    in.age = 0;
    in.zero = 0;
  }
  if (tail != in.tail.load(std::memory_order_relaxed)) {
    in.tail.store(tail, std::memory_order_relaxed);
    h.graph_->set_insert_tail(in.track, in.slot, tail);
  }
  in.acc += h.cycles_() - t0;
}

// Steps 2 to 6 of OS-INSFX-ABI2 3.2. The two float planes are the graph's (the master chain's, free while the strips run): L and R in, render_st in place, then M' and S' in the same two planes before the Q15 conversion.
// Every float here is exact for an identity effect: M + S is a 17-bit integer, times 2^-15; halving the sum and the difference gives M and S back; the module rule (to_q15) cuts towards zero as for a mono effect.
// The input S block may be the shared block itself (a second stereo insert of the strip): it is read in full before the planes are rendered, and written only after the blend, which reads it once more per sample.
std::uint32_t InsertHost::stereo_block(Inst& in, std::int16_t* io, std::uint32_t n, const std::int16_t* sin, std::uint32_t g0, std::uint32_t g1, bool ramp, bool& s_zero) noexcept {
  float* const pl = graph_->insert_scratch(0);
  float* const pr = graph_->insert_scratch(1);
  constexpr float kQ = 1.0f / 32768.0f;  // 2^-15
  const float gf0 = static_cast<float>(g0) / 255.0f;  // (a division, not a multiply by 1/255: 255 / 255 is exactly 1, so a steady S is scaled by exactly 1)
  const float dg = (static_cast<float>(g1) / 255.0f - gf0) / static_cast<float>(n);
  const bool unity = g0 == 255u && g1 == 255u;  // a steady S at its full share: gs is exactly 1 (and dg exactly 0), so the per-sample multiply is skipped (x * 1.0f is x: no bit changes)
  // 2. In: L = (M + S) / 32768, R = (M - S) / 32768; S is the input's share after the mono inserts before this one (a steady one gives none: sin is null).
  if (sin != nullptr) {
    if (unity) {
      for (std::uint32_t i = 0; i < n; ++i) {
        const float m = static_cast<float>(io[i]);
        const float s = static_cast<float>(sin[i]);
        pl[i] = (m + s) * kQ;
        pr[i] = (m - s) * kQ;
      }
    } else {
      float gs = gf0;
      for (std::uint32_t i = 0; i < n; ++i) {
        const float m = static_cast<float>(io[i]);
        const float s = static_cast<float>(sin[i]) * gs;
        gs += dg;
        pl[i] = (m + s) * kQ;
        pr[i] = (m - s) * kQ;
      }
    }
  } else {
    for (std::uint32_t i = 0; i < n; ++i) {
      const float m = static_cast<float>(io[i]) * kQ;
      pl[i] = m;
      pr[i] = m;
    }
  }
  // 3. The module's stereo render, between the platform's stamps.
  const std::uint16_t prev = enter(in);
  const std::uint32_t r = in.mod->render_st(in.state, pl, pr, pl, pr, n);
  leave(prev);
  in.idle = r == 0u;
  // 4 and 5. A transition blends the dry pair (from io and the input S, both still intact) with the effect's, per sample, over the block; then M' = (L + R) / 2 and S' = (L - R) / 2.
  if (ramp) {
    float m = in.mix;
    const float dm = (in.target - m) / static_cast<float>(n);
    float gs = gf0;
    for (std::uint32_t i = 0; i < n; ++i) {
      const float mi = static_cast<float>(io[i]);
      const float si = sin == nullptr ? 0.0f : (unity ? static_cast<float>(sin[i]) : static_cast<float>(sin[i]) * gs);
      gs += dg;
      const float dl = (mi + si) * kQ;
      const float dr = (mi - si) * kQ;
      m += dm;
      const float wl = dl + (pl[i] - dl) * m;
      const float wr = dr + (pr[i] - dr) * m;
      pl[i] = (wl + wr) * 0.5f;
      pr[i] = (wl - wr) * 0.5f;
    }
    in.mix = in.target;
  } else {
    for (std::uint32_t i = 0; i < n; ++i) {
      const float l = pl[i];
      const float rr = pr[i];
      pl[i] = (l + rr) * 0.5f;
      pr[i] = (l - rr) * 0.5f;
    }
  }
  stats_.saturated += to_q15(io, pl, n);
  stats_.saturated += to_q15(sblk_, pr, n);
  // 6. The strip's S is S' (null when it is all zero: side_pass has nothing to add, and a stale pointer to a block another track is about to overwrite cannot exist).
  s_zero = block_silent(sblk_, n);
  if (in.track < kTracks) {
    if (s_zero) {
      graph_->feed_side(in.track, nullptr);
    } else {
      graph_->feed_side(in.track, sblk_);
      s_used_ = static_cast<std::uint16_t>(s_used_ | (1u << in.track));
    }
  }
  return r;
}

// OS-INSFX S6: the master insert, the Graph's MasterFn of MasterStage::Insert: the left plane through the left instance, the right plane through the right one (`state` is the left record, which names its mate).
// OS-INSFX-ABI2 A7: a stereo module is ONE instance (`mate` kNone): run_master_st renders both planes at once.
void InsertHost::run_master(void* state, float* left, float* right, std::uint32_t n) noexcept {
  Inst& l = *static_cast<Inst*>(state);
  InsertHost& h = *l.host;
  if (n == 0u || n > kBlock) {
    return;
  }
  if (l.mate == kNone) {  // A7: the master's one stereo instance (a mono module always binds the pair, so a lone record here is stereo)
    h.run_master_st(l, left, right, n);
    return;
  }
  h.run_plane(l, left, n);
  h.run_plane(h.in_[l.mate], right, n);
}

// A7 (spec 3.3): the master's stereo instance: render_st on the two float planes, then the S6 scrub of NaN and infinity on both, so neither reaches the limiter or the converter. One record, one state block. Steady,
// render_st runs IN PLACE (l, r, l, r, n: no copy, identity bit for bit); in a TRANSITION block only (bind, bypass on/off, unbind, a net's forced bypass: mix != target) it renders into the graph's two chain-wet
// planes (chain_wet(0)/chain_wet(1): the CUE bus's accumulators, which finish_bus has consumed and zeroed before the chain runs — no SRAM cost) and the dry planes and the wet are blended per sample over the block,
// the same one-block linear ramp the dual-mono pair runs — no edge steps. The chain-wet planes are zeroed again right after the blend: acc_cue_ must be zero between blocks, the next block accumulates into it.
void InsertHost::run_master_st(Inst& in, float* left, float* right, std::uint32_t n) noexcept {
  const std::uint32_t t0 = cycles_();
  if (in.idle && plane_silent(left, n) && plane_silent(right, n)) {  // the effect said silent and idle, and has nothing to add to silence
    in.mix = in.target;
    in.acc += cycles_() - t0;
    return;
  }
  if (in.mix == 0.0f && in.target == 0.0f) {  // a ramp that left before it ever ran: dry, the effect is not called
    return;
  }
  const bool ramp = in.mix != in.target;
  float* const wl = ramp ? graph_->chain_wet(0) : left;   // steady: in place, no copy
  float* const wr = ramp ? graph_->chain_wet(1) : right;
  const std::uint16_t prev = enter(in);
  const std::uint32_t r = in.mod->render_st(in.state, left, right, wl, wr, n);
  leave(prev);
  in.idle = r == 0u;
  stats_.scrubbed += scrub_plane(wl, n) + scrub_plane(wr, n);  // before the blend: a NaN in the wet must not poison it
  if (ramp) {  // one block, linear: the dry planes and the effect's, never a step
    float m = in.mix;
    const float dm = (in.target - m) / static_cast<float>(n);
    for (std::uint32_t i = 0; i < n; ++i) {
      m += dm;
      left[i] += (wl[i] - left[i]) * m;
      right[i] += (wr[i] - right[i]) * m;
    }
    in.mix = in.target;
    // the chain-wet planes ARE the CUE bus's accumulator (Graph::chain_wet over acc_cue_), which must be zero between blocks: finish_bus zeroed it before the chain, the next block accumulates into it
    // again. Zero both planes again here (128 int32 stores, transition blocks only), or the next block's CUE output is the float bit patterns as full-scale noise.
    std::memset(wl, 0, kBlock * sizeof(float));
    std::memset(wr, 0, kBlock * sizeof(float));
  }
  in.acc += cycles_() - t0;
}

// One plane through one instance, in place (the planes are float already: no conversion). Steady, the module renders into the plane itself (the ABI allows in == out); in a transition block it renders into the host's
// wet block and the plane (the dry signal) and the wet are mixed over the block, as the track insert does it. Either way the module's output is SCRUBBED before anything else sees it (NaN, infinity: 0).
void InsertHost::run_plane(Inst& in, float* plane, std::uint32_t n) noexcept {
  const std::uint32_t t0 = cycles_();
  const bool silent_in = plane_silent(plane, n);
  if (in.idle && silent_in) {  // the effect said silent and idle, and has nothing to add to silence
    in.mix = in.target;
    in.acc += cycles_() - t0;
    return;
  }
  if (in.mix == 0.0f && in.target == 0.0f) {  // a ramp that left before it ever ran: dry from the first sample, the effect is not called
    return;
  }
  const bool ramp = in.mix != in.target;
  float* const out = ramp ? wet_ : plane;
  const std::uint16_t prev = enter(in);
  const std::uint32_t r = in.mod->render(in.state, plane, out, n);
  leave(prev);
  in.idle = r == 0u;
  stats_.scrubbed += scrub_plane(out, n);
  if (ramp) {  // one block, linear: the dry plane and the effect's, never a step
    float m = in.mix;
    const float dm = (in.target - m) / static_cast<float>(n);
    for (std::uint32_t i = 0; i < n; ++i) {
      m += dm;
      plane[i] += (out[i] - plane[i]) * m;
    }
    in.mix = in.target;
  }
  in.acc += cycles_() - t0;
}

}  // namespace o3os::audio
