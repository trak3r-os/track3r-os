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

bool InsertHost::admit(std::uint32_t cost) const noexcept {
  const gov::Governor& g = voices_->governor();
  std::uint32_t sounding = 0;  // the module voices that sound (their figure, or the cold assumption): the governor's `base` holds the inserts but not these
  for (unsigned k = 0; k < kMaxModuleInstances; ++k) {
    ModuleVoices::Info mi;
    if (voices_->info(k, mi) && mi.active) {
      sounding += mi.cost != 0u ? mi.cost : gov::kColdCycles;
    }
  }
  const std::uint32_t base = g.base();
  const std::uint32_t gone = gone_.load(std::memory_order_relaxed);  // inserts that stopped running but that the base still holds
  return gov::decide((base > gone ? base - gone : 0u) + sounding, cost != 0u ? cost : gov::kColdInsertCycles, false, g.threshold()) == gov::Verdict::Admit;
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

InsertHost::Bind InsertHost::bind(unsigned where, const o3m_module& mod, std::uint16_t fault_id, void* state, const std::uint8_t* params, std::uint32_t param_len,
                                  bool force) noexcept {
  if (graph_ == nullptr || voices_ == nullptr || blobs_ == nullptr) {
    return Bind::NoGraph;
  }
  if (where >= kWhere) {
    return Bind::BadWhere;
  }
  if (wh_[where] != kNone) {
    return Bind::WhereTaken;
  }
  if (mod.kind != O3M_KIND_EFFECT || mod.render == nullptr || mod.init == nullptr || mod.shutdown == nullptr) {
    return Bind::BadModule;
  }
  Inst* pick = nullptr;
  if (state == nullptr) {
    for (unsigned i = 0; i < kInstances && pick == nullptr; ++i) {  // the free list: a record that held this module keeps its state block
      if (in_[i].ml == Ml::Free && in_[i].mod == &mod && in_[i].state != nullptr) {
        pick = &in_[i];
      }
    }
    if (pick == nullptr) {
      for (unsigned i = 0; i < kInstances; ++i) {
        if (in_[i].ml == Ml::Free) {
          return Bind::NeedState;
        }
      }
      return Bind::NoInstance;
    }
  } else {
    if ((reinterpret_cast<std::uintptr_t>(state) & 15u) != 0u) {
      return Bind::BadState;
    }
    for (unsigned i = 0; i < kInstances && pick == nullptr; ++i) {  // a never used record first, then one of another module (its state block stays lost until the arena is reset)
      pick = (in_[i].ml == Ml::Free && in_[i].mod == nullptr) ? &in_[i] : nullptr;
    }
    for (unsigned i = 0; i < kInstances && pick == nullptr; ++i) {
      pick = in_[i].ml == Ml::Free ? &in_[i] : nullptr;
    }
    if (pick == nullptr) {
      return Bind::NoInstance;
    }
  }
  const bool reuse = state == nullptr;
  if (!force && !admit(reuse ? pick->est.load(std::memory_order_relaxed) : 0u)) {
    return Bind::NoCpu;
  }
  if (!reuse) {
    pick->mod = &mod;
    pick->state = state;
    pick->est.store(0, std::memory_order_relaxed);
  }
  pick->fault_id = fault_id;
  pick->set_mask = 0;
  if (!init_module(*pick)) {
    if (!reuse) {
      pick->mod = nullptr;
      pick->state = nullptr;
    }
    return Bind::InitFailed;
  }
  start_binding(*pick, where, params, param_len);
  const Command c{Cmd::Activate, static_cast<std::uint8_t>(pick - in_), static_cast<std::uint8_t>(where >> 1), static_cast<std::uint8_t>(where & 1u), pick->gen, 0, 0};
  pick->act_pushed = push(c);
  return Bind::Ok;
}

bool InsertHost::unbind(unsigned where) noexcept {
  if (where >= kWhere || wh_[where] == kNone || in_[wh_[where]].ml != Ml::Bound) {
    return false;
  }
  Inst& in = in_[wh_[where]];
  in.ml = Ml::Leaving;
  in.then_reactivate = false;
  service();
  return true;
}

InsertHost::Toggle InsertHost::set_bypass(unsigned where, bool on, bool force) noexcept {
  if (where >= kWhere || wh_[where] == kNone || in_[wh_[where]].ml != Ml::Bound) {
    return Toggle::NotBound;
  }
  Inst& in = in_[wh_[where]];
  if (on) {
    in.byp_want = true;
    service();
    return Toggle::Done;
  }
  if (!in.byp_want && in.forced.load(std::memory_order_relaxed) == 0u) {
    return Toggle::Done;  // not bypassed
  }
  if (!force && !admit(in.est.load(std::memory_order_relaxed))) {
    return Toggle::NoCpu;
  }
  in.ml = Ml::Leaving;  // a reset: the state is the module's fresh one, through the swap protocol
  in.then_reactivate = true;
  service();
  return Toggle::Done;
}

bool InsertHost::set_param(unsigned where, unsigned id, float value) noexcept {
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

void InsertHost::finish_leave(Inst& in) noexcept {
  const unsigned where = in.where;
  if (!in.then_reactivate) {
    shutdown_module(in);
  }
  if (!in.then_reactivate || !restart_module(in)) {
    clear_where(in);
    in.ml = Ml::Free;  // the record keeps its module and state block: the next bind of the same module reuses it
    return;
  }
  start_binding(in, where, nullptr, 0);  // (set_mask is kept: no params; the stored values were applied by restart_module, which start_binding's dirty = 0 follows)
}

void InsertHost::service() noexcept {
  if (graph_ == nullptr) {
    return;
  }
  for (unsigned i = 0; i < kInstances; ++i) {
    Inst& in = in_[i];
    if (in.ml == Ml::Bound) {
      if (!in.act_pushed) {
        const Command c{Cmd::Activate, static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(in.where >> 1), static_cast<std::uint8_t>(in.where & 1u), in.gen, 0, 0};
        in.act_pushed = push(c);
        continue;
      }
      if (in.byp_want && !in.byp_pushed) {
        const Command c{Cmd::Bypass, static_cast<std::uint8_t>(i), 0, 0, in.gen, 1, 0};
        in.byp_pushed = push(c);
      }
      flush_params(in);
    } else if (in.ml == Ml::Leaving) {
      bool done = !in.act_pushed;  // the render never saw this binding
      if (!done) {
        if (!in.deact_pushed) {
          const Command c{Cmd::Deactivate, static_cast<std::uint8_t>(i), 0, 0, in.gen, 0, 0};
          in.deact_pushed = push(c);
        }
        done = in.deact_pushed && retired_ok(in);
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
    if (graph_ != nullptr && in.phase != Phase::Off) {  // only a forced release gets here with the render holding it (settled() is the rule): the slot must not outlive the module
      graph_->clear_insert(in.track, in.slot);
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
    in.set_mask = 0;
    in.dirty = 0;
    in.est.store(0, std::memory_order_relaxed);
  }
  for (unsigned w = 0; w < kWhere; ++w) {
    wh_[w] = kNone;
  }
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
      }
      in.idle = false;
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
  gone_.store(0, std::memory_order_relaxed);
  nets_ = false;
  return true;
}

void InsertHost::export_end() noexcept {
  nets_ = true;
  agg_over_ = 0;
  gone_.store(0, std::memory_order_relaxed);  // account() does not decay it while the nets are off: a figure left from the export would outlive it
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
  out.gen = in.gen;
  out.cost = in.est.load(std::memory_order_relaxed);
  return true;
}

std::uint32_t InsertHost::cost(unsigned where) const noexcept {
  return where < kWhere && wh_[where] != kNone ? in_[wh_[where]].est.load(std::memory_order_relaxed) : 0u;
}

// ---- render context -------------------------------------------------------------------------------------------------------------------------

// The wet share goes from where it is to dry or to wet over the coming block; the Graph is told for the stereo side.
void InsertHost::leave_ramp(Inst& in, Phase to) noexcept {
  if ((in.phase == Phase::In || in.phase == Phase::On) && in.prev != 0u) {  // it stops running: what it took is in the base for a while yet, and the nets and admit() must not count it
    gone_.store(gone_.load(std::memory_order_relaxed) + in.est.load(std::memory_order_relaxed), std::memory_order_relaxed);
  }
  graph_->set_insert_wet(in.track, in.slot, wet_of(in.mix), 0u);
  in.target = 0.0f;
  in.phase = to;
  in.pub_phase.store(static_cast<std::uint8_t>(to), std::memory_order_relaxed);
}

void InsertHost::force_bypass(Inst& in) noexcept {
  in.forced.store(1, std::memory_order_relaxed);
  leave_ramp(in, Phase::OutBypass);
}

void InsertHost::apply(const Command& c) noexcept {
  if (c.inst >= kInstances) {
    return;
  }
  Inst& in = in_[c.inst];
  if (c.gen != in.cur_gen.load(std::memory_order_relaxed)) {  // queued for a binding that has since been released (release_all with no render) and whose record was bound again
    return;
  }
  switch (c.kind) {
    case Cmd::Activate: {
      if (in.phase != Phase::Off || c.track >= kTracks || c.slot > 1u || in.mod == nullptr) {
        return;
      }
      const unsigned where = c.track * 2u + c.slot;
      if (rwh_[where] != kNone) {
        return;
      }
      in.rgen = c.gen;
      in.track = c.track;
      in.slot = c.slot;
      rwh_[where] = c.inst;
      in.idle = false;
      in.mix = 0.0f;
      in.target = 1.0f;
      in.acc = 0;
      in.prev = 0;
      in.over = 0;
      in.est_at = blocks_.load(std::memory_order_relaxed);
      {  // it runs again: the base takes its cycles back, so they are no longer "gone" (the estimate is kept across a bypass off or a re-bind)
        const std::uint32_t est = in.est.load(std::memory_order_relaxed);
        const std::uint32_t gone = gone_.load(std::memory_order_relaxed);
        gone_.store(gone > est ? gone - est : 0u, std::memory_order_relaxed);
      }
      in.phase = Phase::In;
      graph_->install_insert(c.track, c.slot, &InsertHost::run, &in);
      graph_->set_insert_bypass(c.track, c.slot, false);
      graph_->set_insert_wet(c.track, c.slot, 0u, 255u);
      in.pub_gen.store(c.gen, std::memory_order_release);
      in.pub_phase.store(static_cast<std::uint8_t>(Phase::In), std::memory_order_relaxed);
      ++stats_.activations;
      break;
    }
    case Cmd::Deactivate:
      if (in.phase != Phase::Off && in.phase != Phase::OutLeave && in.rgen == c.gen) {
        leave_ramp(in, Phase::OutLeave);
      }
      break;
    case Cmd::Bypass:
      if (c.on != 0u && (in.phase == Phase::In || in.phase == Phase::On) && in.rgen == c.gen) {
        leave_ramp(in, Phase::OutBypass);
      }
      break;
  }
}

bool InsertHost::apply_param(const core::ParamChange& c) noexcept {
  const unsigned where = core::param_index(c.id);
  const unsigned field = core::param_field(c.id);
  const std::uint8_t idx = where < kWhere ? rwh_[where] : kNone;
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
  const std::uint32_t gone = gone_.load(std::memory_order_relaxed);
  gone_.store(gone - (gone >> gov::kWorstDecayShift), std::memory_order_relaxed);  // the base's own rate of fall: base - gone stays what the others take
  std::uint32_t total = 0;
  std::uint32_t est_sum = 0;
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
    est_sum += est;
    if ((in.phase == Phase::In || in.phase == Phase::On) && (est != 0u ? est : sample) >= worst_cost) {
      worst = &in;
      worst_cost = est != 0u ? est : sample;
    }
  }
  if (!any) {
    agg_over_ = 0;
    return;
  }
  total_pub_.store(total, std::memory_order_relaxed);
  // The aggregate net: the inserts together against what the threshold leaves once the rest of the render (the base, which holds them, less those that just stopped) is taken off, and never above the
  // instance budget (so all the inserts together take at most the budget: spec 6).
  const std::uint32_t base = g.base();
  const std::uint32_t held = est_sum + gone_.load(std::memory_order_relaxed);  // the running inserts, and the ones the base has not yet forgotten
  const std::uint32_t others = base > held ? base - held : 0u;
  const std::uint32_t threshold = g.threshold();
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
  const std::uint32_t now = blocks_.load(std::memory_order_relaxed) + 1u;
  blocks_.store(now, std::memory_order_release);
  // 1. The transitions of the block that just ran end here, whether or not run() was reached (an idle strip never calls it again): a slot that was ramping out is retired.
  for (unsigned i = 0; i < kInstances; ++i) {
    Inst& in = in_[i];
    switch (in.phase) {
      case Phase::In:
        if (in.mix == in.target) {  // the ramp ran (a strip that was idle has not heard the effect yet: it fades in when it first runs)
          graph_->set_insert_wet(in.track, in.slot, 255u, 255u);
          in.phase = Phase::On;
          in.pub_phase.store(static_cast<std::uint8_t>(Phase::On), std::memory_order_relaxed);
        }
        break;
      case Phase::OutBypass:
        graph_->set_insert_bypass(in.track, in.slot, true);
        graph_->set_insert_wet(in.track, in.slot, 255u, 255u);
        in.mix = 0.0f;
        in.phase = Phase::Bypassed;
        in.pub_phase.store(static_cast<std::uint8_t>(Phase::Bypassed), std::memory_order_relaxed);
        break;
      case Phase::OutLeave:
        graph_->clear_insert(in.track, in.slot);
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
  // 2. The block's measures, the nets.
  account();
  // 3. What the main loop sent since the last boundary.
  Command c;
  for (std::uint32_t n = 0; n < kMaxCommandsPerBlock && ring_.pop(c); ++n) {
    apply(c);
  }
}

// The Graph's InsertFn: in place on the track's mono Q15 block.
void InsertHost::run(void* state, std::int16_t* io, std::uint32_t n) noexcept {
  Inst& in = *static_cast<Inst*>(state);
  InsertHost& h = *in.host;
  if (n == 0u || n > kBlock) {
    return;
  }
  const std::uint32_t t0 = h.cycles_();
  if (in.idle && block_silent(io, n)) {  // the effect said silent and idle and has nothing to add to silence
    in.mix = in.target;
    in.acc += h.cycles_() - t0;
    return;
  }
  if (in.mix == 0.0f && in.target == 0.0f) {  // a ramp that left before it ever ran (or a bypass / unbind in the block of its Activate): dry from the first sample, the effect is not called
    return;
  }
  float* const fl = h.graph_->insert_scratch(0);
  const bool ramp = in.mix != in.target;
  kernel::q15_to_float(fl, io, n);
  float* dry = nullptr;
  if (ramp) {
    dry = h.graph_->insert_scratch(1);
    for (std::uint32_t i = 0; i < n; ++i) {
      dry[i] = fl[i];
    }
  }
  const std::uint16_t prev = h.enter(in);
  const std::uint32_t r = in.mod->render(in.state, fl, fl, n);
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
  in.acc += h.cycles_() - t0;
}

}  // namespace o3os::audio
