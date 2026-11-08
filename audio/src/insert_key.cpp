// SPDX-License-Identifier: GPL-3.0-or-later
// OS-INSFX-ABI2 A6 (docs/specs/OS-INSFX-ABI2.md 3.5): the sidechain key of the insert host. Every function here runs in the render context (the block boundary and the Graph's strip pass), in flash, with no
// allocation, no call out and bounded loops (16 tracks, 4 instances, 2 key parameters, 64 samples). The design is in audio/insert_host.h (THE SIDECHAIN KEY) and module/key_state.h.
#include <o3os/audio/insert_host.h>

namespace o3os::audio {

namespace {

// The peak magnitude of a Q15 block, 0..32768.
std::uint32_t block_peak(const std::int16_t* io, std::uint32_t n) noexcept {
  std::uint32_t peak = 0;
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::int32_t v = io[i];
    const std::uint32_t a = static_cast<std::uint32_t>(v < 0 ? -v : v);
    peak = a > peak ? a : peak;
  }
  return peak;
}

// The key track of a KEY parameter's value: 0 none, 1..16 = track + 1 (the parameter is an INT 0..16; a NaN or a value outside it is held to the range). A key naming the effect's own
// track (`self_track`, 0..15) is 0: the tap measures the track's block after its inserts, so the effect would read its own post-duck output back — a feedback loop that pumps.
std::uint8_t key_of(float v, std::uint8_t self_track) noexcept {
  if (!(v >= 0.5f)) {  // (NaN included)
    return 0u;
  }
  const std::uint8_t k = v >= 16.0f ? std::uint8_t{16} : static_cast<std::uint8_t>(static_cast<unsigned>(v + 0.5f));
  return k == static_cast<std::uint8_t>(self_track + 1u) ? 0u : k;
}

}  // namespace

// The key tap: the graph's InsertFn of an empty slot B on a keyed track. `state` is the track's word in module::g_keys. It writes the block's peak and leaves the block as it was.
void InsertHost::key_tap(void* state, std::int16_t* io, std::uint32_t n) noexcept {
  if (n == 0u || n > kBlock) {
    return;
  }
  static_cast<std::atomic<std::uint32_t>*>(state)->store((block_peak(io, n) & module::KeyLevels::kPeak) | module::KeyLevels::kRan, std::memory_order_relaxed);
}

// The InsertFn of a module in slot B: the module's own run(), then the peak of what it left, while the track is a key (the boundary's mask; the module may be idle, bypassing or ringing: its output is what the fader gets).
void InsertHost::run_keyed(void* state, std::int16_t* io, std::uint32_t n) noexcept {
  run(state, io, n);
  const Inst& in = *static_cast<Inst*>(state);
  if (n != 0u && n <= kBlock && in.track < kTracks && ((module::g_keys.mask.load(std::memory_order_relaxed) >> in.track) & 1u) != 0u) {
    module::g_keys.store(in.track, block_peak(io, n));
  }
}

// The key tracks of a record that the render has just taken (Activate): its KEY parameters' values from the blob the main loop wrote before it pushed the command (a parameter never set is the descriptor's default).
void InsertHost::read_keys(Inst& in) noexcept {
  in.key[0] = 0u;
  in.key[1] = 0u;
  if (in.mod == nullptr || !key_ok(*in.mod) || blobs_ == nullptr) {
    return;
  }
  const unsigned idx = static_cast<unsigned>(&in - in_);
  const std::uint32_t n = in.mod->param_count < kParams ? in.mod->param_count : kParams;
  unsigned k = 0;
  for (std::uint32_t i = 0; i < n && k < 2u; ++i) {
    if ((in.mod->params[i].flags & O3M_PARAM_KEY) != 0u) {
      in.key[k++] = key_of(((in.set_mask >> i) & 1u) != 0u ? (*blobs_)[idx][i] : in.mod->params[i].def, in.track);
    }
  }
}

// A ParamChange reached parameter `field`: when it is a KEY parameter, it is the instance's key now.
void InsertHost::set_key(Inst& in, unsigned field, float value) noexcept {
  if (in.mod == nullptr || !key_ok(*in.mod) || field >= in.mod->param_count || (in.mod->params[field].flags & O3M_PARAM_KEY) == 0u) {
    return;
  }
  unsigned k = 0;
  for (unsigned i = 0; i < field; ++i) {
    k += (in.mod->params[i].flags & O3M_PARAM_KEY) != 0u ? 1u : 0u;
  }
  if (k < 2u) {
    in.key[k] = key_of(value, in.track);
  }
}

// The block boundary (end of begin_block): the words of the block that ended age (measured then: the previous block's now), the key mask is the tracks the running instances name, a track that left it is forgotten,
// and slot B of every keyed track holds the tap unless a module that runs holds it (a bypassed module's slot gets the tap over it: an un-bypass is a reset and a new Activate that takes the slot back; a module that
// leaves clears the slot and the tap goes back). A track that is not a key has no tap. Nothing to do while no instance names a track and no tap is left.
void InsertHost::key_boundary() noexcept {
  std::uint16_t mask = 0;
  for (unsigned i = 0; i < kInstances; ++i) {
    const Inst& in = in_[i];
    if (in.phase == Phase::Off || in.phase == Phase::Bypassed) {  // (a forced or a user bypass has ended its ramp: the effect no longer reads its key)
      continue;
    }
    for (unsigned k = 0; k < 2u; ++k) {
      if (in.key[k] != 0u) {
        mask = static_cast<std::uint16_t>(mask | (1u << (in.key[k] - 1u)));
      }
    }
  }
  if (mask == 0u && key_prev_ == 0u && tap_mask_ == 0u) {
    return;
  }
  module::g_keys.age(static_cast<std::uint16_t>(mask & key_prev_));
  module::g_keys.forget(static_cast<std::uint16_t>(key_prev_ & ~mask));
  module::g_keys.mask.store(mask, std::memory_order_relaxed);
  key_prev_ = mask;
  for (unsigned t = 0; t < kTracks; ++t) {
    const std::uint8_t idx = rwh_[t * 2u + 1u];
    const bool want = ((mask >> t) & 1u) != 0u && (idx == kNone || in_[idx].phase == Phase::Bypassed);
    const bool have = ((tap_mask_ >> t) & 1u) != 0u;
    if (want && !have) {
      graph_->install_insert(t, 1u, &InsertHost::key_tap, &module::g_keys.word[t]);
      graph_->set_insert_bypass(t, 1u, false);
      graph_->set_insert_wet(t, 1u, 0u, 0u);  // the tap changes nothing: side_pass takes the strip's S whole (255 - wet)
      tap_mask_ = static_cast<std::uint16_t>(tap_mask_ | (1u << t));
    } else if (!want && have) {
      graph_->clear_insert(t, 1u);
      tap_mask_ = static_cast<std::uint16_t>(tap_mask_ & ~(1u << t));
    }
  }
}

}  // namespace o3os::audio
