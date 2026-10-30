// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/audio/voices.h>
#include <o3os/audio/lfo_sine.h>
#include <o3os/audio/pitch_ramp.h>

#include <o3os/audio/simd.h>

#include <cstring>

namespace o3os::audio {
namespace {

constexpr unsigned kNone = kMaxVoices;

O3OS_AUDIO_INLINE std::int16_t sat16(std::int32_t v) noexcept { return static_cast<std::int16_t>(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }

O3OS_AUDIO_INLINE std::int32_t abs32(std::int32_t v) noexcept { return v < 0 ? -v : v; }

// The cutoff ADSR's milliseconds in samples at the engine's rate (44.1176 a ms at the default rate).
constexpr float kSamplesPerMs = static_cast<float>(hal::audio::sample_rate_mhz()) * 1.0e-6f;
static_assert(VoicePool::kBlock % kFilterCtl == 0u, "the filter modulation's grid divides the block");

// `v` held to [lo, hi] (a NaN is not held: callers pass finite values).
O3OS_AUDIO_INLINE float clampf(float v, float lo, float hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

// A field's value as the instrument's tables hold it (OS-LIVEP): a whole number in [lo, hi], rounded to the nearest.
O3OS_AUDIO_INLINE int whole(float v, int lo, int hi) noexcept {
  return v <= static_cast<float>(lo) ? lo : (v >= static_cast<float>(hi) ? hi : static_cast<int>(v + (v < 0.0f ? -0.5f : 0.5f)));
}

// The cutoff modulator's amount / sustain as set_sample() holds them: a NaN is 0 / 1.
O3OS_AUDIO_INLINE float mod_amount(float v) noexcept { return v >= -1.0f ? (v <= 1.0f ? v : 1.0f) : (v < -1.0f ? -1.0f : 0.0f); }
O3OS_AUDIO_INLINE float mod_sustain(float v) noexcept { return v >= 0.0f ? (v <= 1.0f ? v : 1.0f) : (v < 0.0f ? 0.0f : 1.0f); }
O3OS_AUDIO_INLINE float clamp01(float v) noexcept { return v >= 1.0f ? 1.0f : (v > 0.0f ? v : 0.0f); }  // NaN is 0

// A modulator's descriptor held to range (on load and when it plays): a NaN amount is 0, a NaN sustain is 1, the shape one of
// the six (a larger one is a triangle), the rate in 0..`max_rate` (28, or the amp table's 23).
void sanitize_mod(FilterMod& fm, unsigned max_rate, bool lfo_only) noexcept {
  fm.kind = fm.kind > kFmodLfo ? kFmodNone : fm.kind;
  fm.kind = (lfo_only && fm.kind == kFmodEnv) ? kFmodNone : fm.kind;
  fm.shape = fm.shape > kLfoShapeSine ? 2u : fm.shape;
  fm.rate = fm.rate > max_rate ? static_cast<std::uint8_t>(max_rate) : fm.rate;
  fm.amount = mod_amount(fm.amount);
  fm.lfo_amount = fm.kind == kFmodLfo ? fm.amount : mod_amount(fm.lfo_amount);  // an LFO instrument: its amount (OS-FU2)
  fm.sustain = mod_sustain(fm.sustain);
}

// A note's gain with the amp envelope's amount (<= 1.0, set_sample) applied: never past kMaxGain.
O3OS_AUDIO_INLINE std::int32_t with_amount(std::int32_t gain, const AmpEnvelope* env) noexcept {
  return env == nullptr ? gain : static_cast<std::int32_t>((static_cast<std::int64_t>(gain) * env->amount + 0x8000) >> 16);
}

// ---- OS-GRAN: the grain shapes (voices.h GRANULAR). The shapes are 1000 entries of u16 gains: the "gauss" one is a half sine, round(65535 sin(pi i / 1000)), and the triangle is
// round(65535 i / 500) up to i = 500 and round(65535 (1000 - i) / 500) after it; both end in 0 (index 999). Here the half sine is its first 501 entries (it is symmetric: gain[i] =
// gain[1000 - i]). No double in this library, so it is a table of constants (1,002 bytes of flash) and not a generator; the tests hold it entry by entry to the formula
// (audio_granular_test.cpp). The triangle is the formula.
constexpr std::uint16_t kHalfSine[501] = {
    0, 206, 412, 618, 824, 1029, 1235, 1441, 1647, 1853, 2059, 2264, 2470, 2676, 2881, 3087,
    3293, 3498, 3704, 3909, 4115, 4320, 4526, 4731, 4937, 5142, 5347, 5552, 5757, 5962, 6167, 6372,
    6577, 6782, 6987, 7191, 7396, 7601, 7805, 8009, 8214, 8418, 8622, 8826, 9030, 9234, 9438, 9641,
    9845, 10049, 10252, 10455, 10658, 10862, 11064, 11267, 11470, 11673, 11875, 12078, 12280, 12482, 12684, 12886,
    13088, 13290, 13491, 13693, 13894, 14095, 14296, 14497, 14698, 14898, 15099, 15299, 15499, 15699, 15899, 16098,
    16298, 16497, 16696, 16895, 17094, 17293, 17491, 17690, 17888, 18086, 18284, 18481, 18679, 18876, 19073, 19270,
    19467, 19663, 19859, 20056, 20251, 20447, 20643, 20838, 21033, 21228, 21423, 21617, 21811, 22005, 22199, 22393,
    22586, 22779, 22972, 23165, 23357, 23550, 23742, 23933, 24125, 24316, 24507, 24698, 24889, 25079, 25269, 25459,
    25649, 25838, 26027, 26216, 26404, 26593, 26781, 26969, 27156, 27343, 27530, 27717, 27903, 28090, 28275, 28461,
    28646, 28831, 29016, 29201, 29385, 29569, 29752, 29936, 30119, 30301, 30484, 30666, 30848, 31029, 31210, 31391,
    31572, 31752, 31932, 32112, 32291, 32470, 32649, 32827, 33005, 33183, 33360, 33537, 33714, 33890, 34066, 34242,
    34417, 34592, 34767, 34941, 35115, 35289, 35462, 35635, 35808, 35980, 36152, 36324, 36495, 36666, 36836, 37006,
    37176, 37345, 37514, 37683, 37851, 38019, 38187, 38354, 38521, 38687, 38853, 39018, 39184, 39349, 39513, 39677,
    39841, 40004, 40167, 40329, 40491, 40653, 40814, 40975, 41136, 41296, 41455, 41615, 41774, 41932, 42090, 42248,
    42405, 42562, 42718, 42874, 43029, 43184, 43339, 43493, 43647, 43800, 43953, 44106, 44258, 44410, 44561, 44711,
    44862, 45012, 45161, 45310, 45459, 45607, 45754, 45901, 46048, 46194, 46340, 46486, 46630, 46775, 46919, 47062,
    47205, 47348, 47490, 47632, 47773, 47914, 48054, 48194, 48333, 48472, 48610, 48748, 48885, 49022, 49159, 49294,
    49430, 49565, 49699, 49833, 49967, 50100, 50232, 50364, 50496, 50627, 50757, 50887, 51017, 51146, 51274, 51402,
    51529, 51656, 51783, 51909, 52034, 52159, 52283, 52407, 52531, 52654, 52776, 52898, 53019, 53140, 53260, 53380,
    53499, 53617, 53736, 53853, 53970, 54087, 54203, 54318, 54433, 54547, 54661, 54775, 54887, 55000, 55111, 55222,
    55333, 55443, 55553, 55662, 55770, 55878, 55985, 56092, 56198, 56304, 56409, 56513, 56617, 56721, 56823, 56926,
    57027, 57129, 57229, 57329, 57429, 57528, 57626, 57724, 57821, 57918, 58014, 58109, 58204, 58298, 58392, 58485,
    58578, 58670, 58761, 58852, 58943, 59032, 59121, 59210, 59298, 59385, 59472, 59558, 59644, 59729, 59813, 59897,
    59980, 60063, 60145, 60227, 60307, 60388, 60467, 60546, 60625, 60703, 60780, 60857, 60933, 61008, 61083, 61158,
    61231, 61304, 61377, 61449, 61520, 61591, 61661, 61730, 61799, 61867, 61935, 62002, 62068, 62134, 62199, 62264,
    62327, 62391, 62454, 62516, 62577, 62638, 62698, 62758, 62817, 62875, 62933, 62990, 63046, 63102, 63158, 63212,
    63266, 63320, 63372, 63425, 63476, 63527, 63577, 63627, 63676, 63724, 63772, 63819, 63866, 63911, 63957, 64001,
    64045, 64089, 64131, 64173, 64215, 64256, 64296, 64335, 64374, 64412, 64450, 64487, 64523, 64559, 64594, 64629,
    64662, 64696, 64728, 64760, 64791, 64822, 64852, 64881, 64910, 64938, 64965, 64992, 65018, 65044, 65069, 65093,
    65116, 65139, 65162, 65183, 65204, 65224, 65244, 65263, 65282, 65299, 65317, 65333, 65349, 65364, 65379, 65392,
    65406, 65418, 65430, 65442, 65452, 65462, 65472, 65480, 65488, 65496, 65503, 65509, 65514, 65519, 65523, 65527,
    65530, 65532, 65534, 65535, 65535,
};

// The grain's amplitude (16 bits) at table index `idx` (0..999) for shape 1 (triangle) or 2 (gauss); 999 and beyond are the table's last entry, 0. Index 950 of the triangle is one below the
// rounded value (6553 where its mirror, index 50, has 6554): a rounding quirk of the reference shape, kept.
O3OS_AUDIO_INLINE std::uint32_t grain_gain(unsigned shape, unsigned idx) noexcept {
  if (idx >= 999u) {
    return 0u;
  }
  const unsigned j = idx <= 500u ? idx : 1000u - idx;
  if (shape == 1u) {
    const std::uint32_t v = (131070u * j + 500u) / 1000u;  // round(65535 j / 500): a multiply and a shift, the divisor is a constant
    return idx == 950u ? v - 1u : v;
  }
  return kHalfSine[j];
}

}  // namespace

VoicePool::VoicePool() noexcept = default;

void VoicePool::set_sample(unsigned slot, const SampleDesc& desc) noexcept {
  if (slot >= kDescSlots) {
    return;
  }
  if (stage_ != nullptr && stage_->slot == slot) {
    (void)stage_cancel();  // OS-BUG-SLICEPITCH: a descriptor staged for this slot is older than this one: the render must not take it over it
  }
  samples_[slot] = sanitized(desc);
}

SampleDesc VoicePool::sanitized(const SampleDesc& desc) noexcept {
  SampleDesc d = desc;
  if (d.data == nullptr || d.length == 0u) {
    d = SampleDesc{};
  } else if (d.loop_end > d.length || d.loop_start >= d.loop_end || d.loop_end - d.loop_start < 2u) {
    d.loop_start = 0;  // a loop that is not inside the sample, or is shorter than 2 samples, is no loop
    d.loop_end = 0;
  }
  d.gain = d.gain < 0 ? 0 : (d.gain > kernel::kMaxGain ? kernel::kMaxGain : d.gain);
  if (d.slices == nullptr || d.slice_count > kMaxSlices) {
    d.slice_count = 0;
    d.slices = nullptr;
  }
  if (d.wt_size != 0u && d.loop_end - d.loop_start < 2u) {
    d.wt_size = 0;  // a window is a loop: without one it is a plain sample
  }
  if (d.wt_size == 0u) {
    d.wt_windows = 0;
    d.wt_pos = 0;
  } else if (d.wt_windows != 0u && d.wt_pos >= d.wt_windows) {
    d.wt_pos = d.wt_windows - 1u;
  }
  d.loop_mode = d.loop_mode > 2u ? 0u : d.loop_mode;
  if (d.granular) {
    d.loop_start = 0;  // the grain is the loop: the instrument's own loop, slices and window are not used (voices.h GRANULAR)
    d.loop_end = 0;
    d.beat_slice = false;
    d.slice_count = 0;
    d.slices = nullptr;
    d.wt_size = 0;
    d.gr_shape = d.gr_shape > 2u ? 0u : d.gr_shape;
  }
  d.pan = d.pan > 100u ? 100u : d.pan;
  d.delay_send = d.delay_send > 100u ? 100u : d.delay_send;
  d.reverb_send = d.reverb_send > 100u ? 100u : d.reverb_send;
  d.glide = d.glide > 15000u ? std::uint16_t{15000} : d.glide;  // OS-IFX1
  d.overdrive = d.overdrive > 100u ? std::uint8_t{100} : d.overdrive;
  d.bit_depth = d.bit_depth > 16u ? std::uint8_t{16} : (d.bit_depth < 4u ? std::uint8_t{4} : d.bit_depth);
  d.filter.wanted = d.filter.wanted || d.filter.enable;  // a descriptor made by hand names the switch only through `enable`
  if (d.filter.type > kFilterBand) {
    d.filter.enable = false;  // an unknown filter type is bypassed here (cutoff and resonance are held to range where they are used, svf_set)
  }
  sanitize_mod(d.filter.mod, kLfoRates - 1u, false);  // (OS-FU2: an LFO instrument's lfo_amount is its amount)
  sanitize_mod(d.pan_mod, kLfoRates - 1u, false);      // OS-MOD
  sanitize_mod(d.tune_mod, kLfoRates - 1u, false);
  sanitize_mod(d.amp_mod, kAmpLfoRates - 1u, true);   // the amp slot is a tremolo or the plain `env`: never an ADSR here
  sanitize_mod(d.pos_mod, kLfoRates - 1u, false);      // OS-GRAN
  if (!(d.granular || (d.wt_size != 0u && d.wt_windows > 1u))) {
    d.pos_mod.kind = kFmodNone;  // the position slots move a granular instrument's grain or a wavetable's window (voices.h POSITION MODULATION)
  }
  AmpEnvelope& e = d.env;
  e.delay = e.delay == 0u ? 1u : e.delay;  // a stage is at least one group of 8 samples
  e.attack = e.attack == 0u ? 1u : e.attack;
  e.hold = e.hold == 0u ? 1u : e.hold;
  e.decay = e.decay == 0u ? 1u : e.decay;
  e.release = e.release == 0u ? 1u : e.release;
  e.sustain = e.sustain > kEnvOne ? kEnvOne : e.sustain;
  e.amount = e.amount < 0 ? 0 : (e.amount > kernel::kUnity ? kernel::kUnity : e.amount);
  return d;
}

// ---- The staged descriptor (voices.h, Staged descriptor) ------------------------------------------------------------------

bool VoicePool::stage_sample(unsigned slot, const SampleDesc& desc, const std::uint16_t* table, unsigned table_words, std::uint16_t* table_dst) noexcept {
  if (stage_ == nullptr || slot >= kDescSlots || stage_state_.load(std::memory_order_acquire) != 0u || table_words > sizeof stage_->table / sizeof stage_->table[0]) {
    return false;
  }
  StageCell& c = *stage_;
  c.slot = static_cast<std::uint8_t>(slot);
  c.desc = sanitized(desc);
  c.table_words = static_cast<std::uint16_t>(table != nullptr && table_dst != nullptr ? table_words : 0u);
  c.table_dst = table_dst;
  for (unsigned i = 0; i < c.table_words; ++i) {
    c.table[i] = table[i];
  }
  stage_state_.store(1, std::memory_order_release);
  return true;
}

bool VoicePool::stage_cancel() noexcept {
  std::uint8_t expected = 1;
  if (stage_state_.compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) {
    return true;
  }
  return expected == 0u;  // already free; 2: the render holds it (it is the one that frees it)
}

// Render context, the first thing of a block (begin_block), once per staged descriptor. The slot's sounding voices end when the new descriptor would make them read other audio or another kind of
// voice than they started with; the slice table is copied in the same step, so no note reads half of it. Nothing here allocates or loops beyond kMaxVoices and the table's 98 words.
void VoicePool::take_stage() noexcept {
  std::uint8_t expected = 1;
  if (stage_ == nullptr || !stage_state_.compare_exchange_strong(expected, 2, std::memory_order_acq_rel)) {
    return;  // cancelled meanwhile
  }
  const StageCell& c = *stage_;
  const unsigned slot = c.slot;
  const SampleDesc& old = samples_[slot];
  const SampleDesc& d = c.desc;
  if (old.data != d.data || old.length != d.length || old.granular != d.granular || old.wt_size != d.wt_size || old.wt_windows != d.wt_windows || old.engine != d.engine) {
    for (unsigned i = 0; i < kMaxVoices; ++i) {
      if (v_[i].playing && v_[i].slot == slot) {
        stop_voice(v_[i]);
      }
    }
  }
  for (unsigned i = 0; i < c.table_words; ++i) {
    c.table_dst[i] = c.table[i];
  }
  samples_[slot] = d;
  stage_takes_.store(stage_takes_.load(std::memory_order_relaxed) + 1u, std::memory_order_release);
  stage_state_.store(0, std::memory_order_release);
}

// ---- The sample loop ----------------------------------------------------------------------------------------------------

// OS-CPU10 slice 2 (A5): the unchecked samples of the forward loop, the body of the checked loop's with its tests taken out (see run). Separate and not inlined so that the allocator has only these
// values to place (data, output pointer and its end, the 64-bit position and increment, amp, and the ramp's step and cap): a loop inside run() spilled three of them to the stack.
template <bool Ramp>
O3OS_AUDIO_INLINE void VoicePool::fast_body(Voice& v, std::int16_t* out, std::uint32_t k, std::int32_t top) noexcept {
  const std::int16_t* const data = v.data;
  // The position as two words, the carry made by hand: the compiler then has no linear 64-bit recurrence to close (it made the exit's position as phase + k x inc from copies of the start, and
  // those copies cost the registers the loop needs).
  const std::uint32_t inc_lo = static_cast<std::uint32_t>(v.inc);
  const std::uint32_t inc_hi = static_cast<std::uint32_t>(v.inc >> 32);
  const std::int32_t step = v.amp_step;
  std::uint32_t lo = static_cast<std::uint32_t>(v.phase);
  std::uint32_t idx = static_cast<std::uint32_t>(v.phase >> 32);
  std::int32_t amp = v.amp;
  std::int16_t* const stop = out + k;
  for (; out != stop; ++out) {
    const std::int32_t s0 = data[idx];
    const std::int32_t s1 = data[idx + 1u];
    const std::int32_t frac = static_cast<std::int32_t>(lo >> 17);
    const std::int32_t s = s0 + (((s1 - s0) * frac) >> 15);
    *out = static_cast<std::int16_t>(simd::NativeOps::ssat16(static_cast<std::int32_t>((static_cast<std::int64_t>(s) * amp + 0x800000) >> 24)));
#if defined(__arm__)
    __asm__("adds %0, %0, %2\n\tadc %1, %1, %3" : "+r"(lo), "+r"(idx) : "r"(inc_lo), "r"(inc_hi) : "cc");  // the 64-bit add as the compiler makes it, in place
#else
    lo += inc_lo;
    idx += inc_hi + (lo < inc_lo ? 1u : 0u);
#endif
    if (Ramp) {
      amp += step;
      amp = amp < 0 ? 0 : (amp > top ? top : amp);
    }
  }
  v.phase = (static_cast<std::uint64_t>(idx) << 32) | lo;
  v.amp = amp;
}

void VoicePool::fast_steady(Voice& v, std::int16_t* out, std::uint32_t k, std::int32_t top) noexcept { fast_body<false>(v, out, k, top); }
void VoicePool::fast_ramp(Voice& v, std::int16_t* out, std::uint32_t k, std::int32_t top) noexcept { fast_body<true>(v, out, k, top); }

template <bool Ramp, bool Dir>
O3OS_AUDIO_INLINE std::uint32_t VoicePool::run(Voice& v, std::int16_t* out, std::uint32_t n) noexcept {
  const std::int16_t* const data = v.data;
  const std::uint64_t inc = v.inc;
  const std::uint32_t end = v.end;
  const std::uint32_t loop_len = v.loop_len;
  const std::uint32_t loop_start = end - loop_len;  // meaningful for a loop only
  const std::int32_t step = v.amp_step;
  const std::int32_t top = (v.gain > v.gain_prev ? v.gain : v.gain_prev) * 256;  // Q24; gain <= kMaxGain (2^20 - 1), so under 2^28; gain_prev is the larger only while a live volume change glides down
  std::uint64_t phase = v.phase;
  std::int32_t amp = v.amp;
  // A backward or ping-pong loop (Dir): the position is walked as a signed Q32.32 number between these limits (see LOOPS in voices.h).
  [[maybe_unused]] const std::int64_t lim_lo = static_cast<std::int64_t>(loop_start) << 32;
  [[maybe_unused]] const std::int64_t lim_hi = static_cast<std::int64_t>(end) << 32;
  [[maybe_unused]] const std::int64_t turn_hi = (static_cast<std::int64_t>(end) * 2 - 1) << 32;         // p' = 2 x loop_end - 1 - p
  [[maybe_unused]] const std::int64_t turn_lo = (static_cast<std::int64_t>(loop_start) * 2 + 1) << 32;  // p' = 2 x loop_start + 1 - p
  [[maybe_unused]] const std::int64_t span = static_cast<std::int64_t>(loop_len) << 32;
  [[maybe_unused]] const bool pingpong = v.mode == 2u;
  [[maybe_unused]] bool bwd = v.bwd;
  [[maybe_unused]] const bool rev = v.mode == kModeReverse;  // OS-PERF-REVERSE: the backward one shot stops below its window's first frame instead of turning
  std::uint32_t i = 0;
  if constexpr (Dir) {
    if (rev && static_cast<std::int64_t>(phase) < lim_lo) {
      return 0;  // the stop came at the last sample of the previous call: nothing is left
    }
    // OS-PERF-CPU2 (A1): the backward twin of the forward fast path below. A voice that moves backward and whose first sample and its neighbour are inside the loop (phase >= loop_start,
    // idx0 + 2 <= end: a loop edit can raise loop_start above the phase of a mode 1 or 2 voice, and the voice's first sample after a jump to the end sits at end - 1) walks k = floor((idx0 - loop_start) / ip)
    // steps with ip = floor(inc) + 1 without a test: the step is below ip samples, so after j <= k steps q_j > (idx0 - j x ip) << 32 >= lim_lo (no stop, no turn, no clamp) and idx <= idx0 <= end - 2
    // (the sample and its neighbour idx + 1 are inside). Those steps are fast_steady / fast_ramp's with the increment negated: q - inc = q + (2^64 - inc) in the same two-word add, exact modulo 2^64
    // (inc 0 and a zero integer part included). The voice's own inc carries the negated value through the call and is put back at once (nothing in between reads it: the render is one context). The
    // step's arithmetic is the checked loop's, so the output and the state are the same bit for bit; the checked loop takes over for the rest (the sample at end - 1, the turn, the stop).
    // phase >= lim_lo (signed) is the signed high word >= loop_start (the low word of lim_lo is 0), which also rejects the negative position a stop leaves; then idx0 < 2^31 and idx0 + 2 cannot wrap. An
    // integer part of the step above 0x7FFFFFFF is held to it: ip = 2^31 > idx0 - loop_start, so k = 0 and the checked loop runs (and ip cannot wrap to 0).
    const std::uint32_t idx0 = static_cast<std::uint32_t>(phase >> 32);
    if (bwd && static_cast<std::int32_t>(idx0) >= static_cast<std::int32_t>(loop_start) && idx0 + 2u <= end) {
      const std::uint32_t inc_hi = static_cast<std::uint32_t>(inc >> 32);
      const std::uint32_t ip_m1 = inc_hi < 0x7FFFFFFFu ? inc_hi : 0x7FFFFFFFu;
      std::uint32_t k = (idx0 - loop_start) / (ip_m1 + 1u);
      k = k < n ? k : n;
      if (k != 0u) {
        v.phase = phase;
        v.amp = amp;
        v.inc = std::uint64_t{0} - inc;
        if (step == 0 && static_cast<std::uint32_t>(amp) <= static_cast<std::uint32_t>(top)) {  // 0 <= amp <= top (top >= 0): the cap and the floor leave amp alone
          fast_steady(v, out, k, top);
        } else {
          fast_ramp(v, out, k, top);
        }
        v.inc = inc;
        phase = v.phase;
        amp = v.amp;
        i = k;
      }
    }
  }
  if constexpr (!Dir) {
    // OS-PERF3: the samples that cannot reach the end of the data (nor its loop point) need none of the checks below. The position moves less than ip = floor(inc) + 1 samples a step, so
    // after j steps idx <= idx0 + j x ip; while that is <= end - 2 both the sample and its neighbour (idx + 1) are inside, the plain next = idx + 1. k = 1 + floor((end - 2 - idx0) / ip)
    // steps qualify (one division a call). The arithmetic of the body is the checked loop's, step for step, so the output is the same bit for bit; the checked loop below takes over
    // for the rest (the last samples of a one shot, a loop wrap).
    const std::uint32_t idx0 = static_cast<std::uint32_t>(phase >> 32);
    if (idx0 + 2u <= end) {
      const std::uint64_t ip64 = (inc >> 32) + 1u;
      const std::uint32_t ip = ip64 > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(ip64);
      std::uint32_t k = 1u + (end - 2u - idx0) / ip;
      k = k < n ? k : n;
      if (k != 0u) {
        v.phase = phase;  // the voice's own fields carry the position and the amplitude through the call
        v.amp = amp;
        if (Ramp) {
          fast_ramp(v, out, k, top);
        } else {
          fast_steady(v, out, k, top);
        }
        phase = v.phase;
        amp = v.amp;
        i = k;
      }
    }
  }
  for (; i < n; ++i) {
    std::uint32_t idx = static_cast<std::uint32_t>(phase >> 32);
    std::uint32_t next;
    if constexpr (Dir) {
      idx = idx < end ? idx : end - 1u;  // the backward loop's jump to the end can land on loop_end itself: that sample is the last one of the loop, held
      next = idx + 1u < end ? idx + 1u : idx;
    } else {
      if (idx >= end) {
        if (loop_len == 0u) {
          break;  // the one shot has played its last sample
        }
        idx = loop_start + (idx - loop_start) % loop_len;  // the one division, at a wrap (UDIV)
        phase = (static_cast<std::uint64_t>(idx) << 32) | static_cast<std::uint32_t>(phase);
      }
      next = idx + 1u;
      if (next >= end) {
        next = loop_len != 0u ? loop_start : idx;  // across the loop point; a one shot holds its last sample
      }
    }
    const std::int32_t s0 = data[idx];
    const std::int32_t s1 = data[next];
    const std::int32_t frac = static_cast<std::int32_t>(static_cast<std::uint32_t>(phase) >> 17);  // Q15, 0..32767
    const std::int32_t s = s0 + (((s1 - s0) * frac) >> 15);                                         // |s1 - s0| < 2^16: no overflow
    // amp is Q24 up to kMaxGain (the instrument law reaches +24 dB): s * amp needs 44 bits, so it is made in 64 (one SMULL) and clipped to int16 like
    // the mixer. A Q16 amp the voice used to hold is this one with 8 zero bits, and the result is the same bit for bit (floor((256 X + 2^23) / 2^24) =
    // floor((X + 2^15) / 2^16)); the extra bits are the envelope's.
    out[i] = sat16(static_cast<std::int32_t>((static_cast<std::int64_t>(s) * amp + 0x800000) >> 24));
    if constexpr (Dir) {
      std::int64_t q = static_cast<std::int64_t>(phase);
      if (!bwd) {
        q += static_cast<std::int64_t>(inc);
        if (q >= lim_hi) {  // the turn at the loop's end: reflect about loop_end - 0.5 and run backward
          q = turn_hi - q;
          q = q < lim_lo ? lim_lo : q;
          bwd = true;
        }
      } else {
        q -= static_cast<std::int64_t>(inc);
        if (q <= lim_lo) {
          if (rev) {  // backward one shot: the window's first frame (q == lim_lo) is still played, the position below it ends the note
            if (q < lim_lo) {
              phase = static_cast<std::uint64_t>(q);
              ++i;  // (this sample is made)
              break;
            }
          } else {
            if (pingpong) {  // reflect about loop_start + 0.5 and run forward
              q = turn_lo - q;
              bwd = false;
            } else {  // backward loop: back to the end, still running backward
              q += span;
            }
            q = q > lim_hi ? lim_hi : (q < lim_lo ? lim_lo : q);  // a step longer than the loop: clamped to its ends
          }
        }
      }
      phase = static_cast<std::uint64_t>(q);
    } else {
      phase += inc;
    }
    if (Ramp) {
      amp += step;
      amp = amp < 0 ? 0 : (amp > top ? top : amp);
    }
  }
  v.phase = phase;
  v.amp = amp;
  if constexpr (Dir) {
    v.bwd = bwd;
  }
  if (i != 0u) {
    v.last = out[i - 1u];
  }
  return i;
}

std::uint32_t VoicePool::run_steady(Voice& v, std::int16_t* out, std::uint32_t n) noexcept { return run<false, false>(v, out, n); }
std::uint32_t VoicePool::run_ramp(Voice& v, std::int16_t* out, std::uint32_t n) noexcept { return run<true, false>(v, out, n); }
std::uint32_t VoicePool::run_dir(Voice& v, std::int16_t* out, std::uint32_t n) noexcept { return run<true, true>(v, out, n); }

// ---- The grain (voices.h GRANULAR). In flash: see the declarations.

// The grain of position `pos` (0..65535) from the descriptor: u16 arithmetic where the position units are u16, in samples counted from the grain's start. Nothing it makes leaves the
// sample, and gl2 > gl1 whatever the descriptor says.
void VoicePool::grain_geom(Voice& v, const SampleDesc& s, unsigned pos) noexcept {
  const std::uint32_t len = s.length;  // >= 1: a granular note needs a sample
  const std::uint16_t units = static_cast<std::uint16_t>(static_cast<std::uint32_t>(s.gr_len) * 65535u / len);  // the grain in position units (a u16)
  const std::int32_t half = units / 2;
  const std::int32_t down = static_cast<std::int32_t>(pos) - half;
  const std::int32_t up = static_cast<std::int32_t>(pos) + half;
  const std::uint16_t g_start = down > 0 ? static_cast<std::uint16_t>(down) : std::uint16_t{0};
  const std::uint16_t g_end = up < 65535 ? static_cast<std::uint16_t>(up) : std::uint16_t{65535};
  const std::uint16_t g_loop1 = static_cast<std::uint16_t>(g_start + 1);  // u16 arithmetic: 65535 + 1 is 0
  const std::uint16_t g_loop2 = static_cast<std::uint16_t>(g_end - 1);    // ... and 0 - 1 is 65535
  const float ratio = static_cast<float>(len) / 65535.0f;
  const auto at = [ratio](std::uint16_t point) { return static_cast<std::uint32_t>(static_cast<float>(point) * ratio); };  // truncated float product with length / 65535
  std::uint32_t start = at(g_start);
  start = start >= len ? len - 1u : start;
  const std::uint32_t p1 = at(g_loop1);
  const std::uint32_t p2 = at(g_loop2);
  const std::uint32_t l1 = p1 > start ? p1 - start : 0u;
  std::uint32_t l2 = p2 > start ? p2 - start : 0u;
  l2 = l2 <= l1 ? l1 + 1u : l2;
  v.gstart = start;
  v.gavail = len - start;
  v.gl1 = l1;
  v.gl2 = l2;
  // floor(k x 999 / (l2 - l1)) for 0 <= k < l2 - l1 is (k x gm) >> 32: exact while k x (l2 - l1) < 2^32, which a grain of under 65536 samples keeps (the grain is at most the
  // sample's length or the u16 length setting). The one 64-bit division of a grain.
  v.gm = (static_cast<std::uint64_t>(999u) << 32) / (l2 - l1) + 1u;
}

// A granular note: the grain at the instrument's position (the position modulator, mods_start, moves it from the first block on), the position in it at 0 (a backward grain runs from there).
void VoicePool::grain_start(Voice& v, const SampleDesc& s) noexcept {
  v.mode = kModeGranular;
  v.gtype = s.loop_mode;
  v.gshape = s.gr_shape;
  v.gpos = s.gr_pos;
  v.gnew = s.gr_pos;
  v.gpend = false;
  grain_geom(v, s, s.gr_pos);
  v.phase = static_cast<std::uint64_t>(v.gstart) << 32;
  v.bwd = s.loop_mode == 1u;
}

void VoicePool::refresh_grain(Voice& v) noexcept {
  v.gpos = v.gnew;
  v.gpend = false;
  grain_geom(v, samples_[v.slot], v.gpos);
}

// n samples of the grain, the amplitude stepping as run_dir's. `Type`: 0 forward, 1 backward, 2 ping-pong. The position q is the grain's x (Q32.32, from gstart), read with the plain
// lerp at the real position; the turns are the reflections of the loop, and a pending position is taken at every wrap or turn.
template <unsigned Type>
O3OS_AUDIO_INLINE std::uint32_t VoicePool::run_gran_t(Voice& v, std::int16_t* out, std::uint32_t n) noexcept {
  constexpr std::int64_t kOne = std::int64_t{1} << 32;
  const std::int16_t* const base = v.data;
  const std::int64_t inc = static_cast<std::int64_t>(v.inc);
  const std::int32_t step = v.amp_step;
  const std::int32_t top = (v.gain > v.gain_prev ? v.gain : v.gain_prev) * 256;
  const unsigned shape = v.gshape;
  std::uint32_t start = v.gstart;
  std::int32_t avail = static_cast<std::int32_t>(v.gavail);
  std::int32_t l1 = static_cast<std::int32_t>(v.gl1);
  std::int32_t span = static_cast<std::int32_t>(v.gl2 - v.gl1);
  std::uint64_t gm = v.gm;
  std::int64_t lo = static_cast<std::int64_t>(l1) << 32;
  std::int64_t hi = static_cast<std::int64_t>(v.gl2) << 32;
  std::int64_t q = static_cast<std::int64_t>(v.phase) - (static_cast<std::int64_t>(start) << 32);
  bool bwd = Type == 1u ? true : (Type == 2u ? v.bwd : false);
  std::int32_t amp = v.amp;
  const auto reload = [&]() {  // the pending position becomes the grain: its points move, the position in the grain does not
    refresh_grain(v);
    start = v.gstart;
    avail = static_cast<std::int32_t>(v.gavail);
    l1 = static_cast<std::int32_t>(v.gl1);
    span = static_cast<std::int32_t>(v.gl2 - v.gl1);
    gm = v.gm;
    lo = static_cast<std::int64_t>(l1) << 32;
    hi = static_cast<std::int64_t>(v.gl2) << 32;
  };
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::int32_t x = static_cast<std::int32_t>(q >> 32);
    const std::int32_t a0 = x < 0 ? 0 : (x >= avail ? avail - 1 : x);  // never a read outside the sample
    const std::int32_t a1 = x + 1 < 0 ? 0 : (x + 1 >= avail ? avail - 1 : x + 1);
    const std::int16_t* const d = base + start;
    const std::int32_t s0 = d[a0];
    const std::int32_t s1 = d[a1];
    const std::int32_t smp = s0 + (((s1 - s0) * static_cast<std::int32_t>(static_cast<std::uint32_t>(q) >> 17)) >> 15);
    // The envelope's index follows the integer position, which runs backward as the ceiling of the real one.
    const std::int32_t k = ((bwd && static_cast<std::uint32_t>(q) != 0u) ? x + 1 : x) - l1;
    const unsigned idx = k <= 0 ? 0u : (k >= span ? 999u : static_cast<unsigned>((static_cast<std::uint64_t>(k) * gm) >> 32));
    const std::int32_t g = (smp * static_cast<std::int32_t>(shape == 0u ? 65535u : grain_gain(shape, idx))) >> 16;  // |smp| <= 32768: 2^31 - 32768 at the most
    out[i] = sat16(static_cast<std::int32_t>((static_cast<std::int64_t>(g) * amp + 0x800000) >> 24));
    if constexpr (Type == 0u) {
      q += inc;
      if (q >= hi) {
        std::int64_t ex = q - hi;
        if (v.gpend) {
          reload();
        }
        const std::int64_t w = hi - lo;
        ex = ex >= w ? static_cast<std::int64_t>(static_cast<std::uint64_t>(ex) % static_cast<std::uint64_t>(w)) : ex;  // a step past a whole grain (the rare one division)
        q = lo + ex;
      }
    } else if constexpr (Type == 1u) {
      q -= inc;
      if (q <= lo) {
        std::int64_t ex = lo - q;
        if (v.gpend) {
          reload();
        }
        const std::int64_t w = hi - lo;
        ex = ex >= w ? static_cast<std::int64_t>(static_cast<std::uint64_t>(ex) % static_cast<std::uint64_t>(w)) : ex;
        q = hi - ex;
      }
    } else {
      if (!bwd) {
        q += inc;
        if (q >= hi) {  // the turn at the top: reflect about loop2 - 0.5 and run backward
          const std::int64_t ex = q - hi;
          if (v.gpend) {
            reload();
          }
          q = hi - kOne - ex;
          q = q < lo ? lo : q;
          bwd = true;
        }
      } else {
        q -= inc;
        if (q <= lo) {  // the turn at the bottom: reflect about loop1 + 0.5 and run forward
          const std::int64_t ex = lo - q;
          if (v.gpend) {
            reload();
          }
          q = lo + kOne + ex;
          q = q > hi ? hi : q;
          bwd = false;
        }
      }
    }
    amp += step;
    amp = amp < 0 ? 0 : (amp > top ? top : amp);
  }
  v.phase = static_cast<std::uint64_t>(q + (static_cast<std::int64_t>(start) << 32));
  v.bwd = bwd;
  v.amp = amp;
  if (n != 0u) {
    v.last = out[n - 1u];
  }
  return n;
}

std::uint32_t VoicePool::run_gran(Voice& v, std::int16_t* out, std::uint32_t n) noexcept {
  switch (v.gtype) {
    case 1: return run_gran_t<1>(v, out, n);
    case 2: return run_gran_t<2>(v, out, n);
    default: return run_gran_t<0>(v, out, n);
  }
}

// The tail of an abrupt stop, added over the samples it covers: its value walks to zero by resid_dec a sample.
void VoicePool::add_residual(Voice& v, std::int16_t* out, std::uint32_t n) noexcept {
  std::int32_t r = v.resid;
  if (r == 0) {
    return;
  }
  const std::int32_t d = v.resid_dec;
  for (std::uint32_t i = 0; i < n; ++i) {
    out[i] = sat16(out[i] + r);
    r = r > 0 ? (r > d ? r - d : 0) : (r < -d ? r + d : 0);
  }
  v.resid = r;
}

// `value` joins the residual, which then fades to zero over kDeclickSamples (at least one unit a sample).
void VoicePool::carry(Voice& v, std::int32_t value) noexcept {
  v.resid += value;
  v.resid_dec = v.resid == 0 ? 0 : (abs32(v.resid) + static_cast<std::int32_t>(kDeclickSamples) - 1) / static_cast<std::int32_t>(kDeclickSamples);
}

// The voice stops producing now: what it last put out becomes a residual, so the output goes to zero without a step.
void VoicePool::stop_voice(Voice& v) noexcept {
  v.playing = false;
  v.releasing = false;
  v.ramp_left = 0;
  v.env_stage = kEnvOff;
  v.amp = 0;
  carry(v, v.last);
  v.last = 0;
}

void VoicePool::start_release(Voice& v, std::uint32_t samples) noexcept {
  if (!v.playing || v.releasing) {
    return;
  }
  v.releasing = true;
  v.ramp_left = samples;
  const std::int32_t s = static_cast<std::int32_t>(samples);
  const std::int32_t step = ((v.amp >> 8) + s - 1) / s;  // from where the voice is (it may be mid attack: whole Q16 units) to silence, the last sample at the latest
  v.amp_step = -(step < 1 ? 1 : step) * 256;
}

// ---- The amp envelope (voices.h, AMP ENVELOPE) ---------------------------------------------------------------------------------

// The voice enters `stage`; `units` is its time in groups of 8 samples (the stages that have one). The level (shape, Q30) and its step a sample are set so that
// the stage runs for exactly the samples it says: a ramp's step is the distance over the length, truncated, so it ends within `length` units of 2^-30 of its
// target and the next stage starts from its exact value.
void VoicePool::env_enter(Voice& v, EnvStage stage, std::uint32_t units) noexcept {
  const std::int32_t sustain = static_cast<std::int32_t>(v.env->sustain);  // set_sample keeps it within [0, kEnvOne]
  const std::uint32_t length = (units < 1u ? 1u : units) * kEnvGroup;
  v.env_stage = stage;
  v.env_left = length;
  switch (stage) {
    case kEnvDelay:
      v.shape = 0;
      v.env_inc = 0;
      break;
    case kEnvAttack:
      v.shape = 0;
      v.env_inc = static_cast<std::int32_t>(kEnvOne / length);
      break;
    case kEnvHold:
      v.shape = static_cast<std::int32_t>(kEnvOne);
      v.env_inc = 0;
      break;
    case kEnvDecay:
      v.shape = static_cast<std::int32_t>(kEnvOne);
      v.env_inc = (sustain - static_cast<std::int32_t>(kEnvOne)) / static_cast<std::int32_t>(length);
      break;
    case kEnvSustain:
      v.env_left = kEnvForever;
      v.shape = sustain;
      v.env_inc = 0;
      break;
    case kEnvRelease:  // from where the level is
      v.env_inc = -(v.shape / static_cast<std::int32_t>(length));
      break;
    default:
      break;
  }
}

// The amplitude over the next n samples of the stage (n <= env_left, so the level cannot leave [0, kEnvOne] by more than the division's remainder): the
// Q24 amplitude at its start, and the step that brings it to the one at its end, from the Q30 level and the Q16 gain. True when it moves.
bool VoicePool::env_segment(Voice& v, std::uint32_t n) noexcept {
  const std::int64_t g = v.gain;
  const std::int64_t g0 = v.gain_prev;  // OS-LIVEP: the gain the level was made for until now; a live volume change makes this segment the ramp from it to `g` (env_advance catches it up)
  const std::int32_t s0 = v.shape;
  std::int32_t s1 = s0 + v.env_inc * static_cast<std::int32_t>(n);
  s1 = s1 < 0 ? 0 : (s1 > static_cast<std::int32_t>(kEnvOne) ? static_cast<std::int32_t>(kEnvOne) : s1);
  const std::int32_t a0 = static_cast<std::int32_t>((static_cast<std::int64_t>(s0) * g0) >> 22);  // Q30 x Q16 = Q46 -> Q24
  const std::int32_t a1 = static_cast<std::int32_t>((static_cast<std::int64_t>(s1) * g) >> 22);
  v.amp = a0;
  v.amp_step = (a1 - a0) / static_cast<std::int32_t>(n);  // the one division of a segment (SDIV), never in the sample loop
  return v.amp_step != 0;
}

// n samples of the stage have been made: move the level on, and enter the next stage when this was the last of the stage.
void VoicePool::env_advance(Voice& v, std::uint32_t n, bool stage_end) noexcept {
  v.gain_prev = v.gain;
  std::int32_t s = v.shape + v.env_inc * static_cast<std::int32_t>(n);
  v.shape = s < 0 ? 0 : (s > static_cast<std::int32_t>(kEnvOne) ? static_cast<std::int32_t>(kEnvOne) : s);
  if (!stage_end) {
    if (v.env_stage != kEnvSustain) {
      v.env_left -= n;
    }
    return;
  }
  const AmpEnvelope& e = *v.env;
  switch (v.env_stage) {
    case kEnvDelay: env_enter(v, kEnvAttack, e.attack); break;
    case kEnvAttack: env_enter(v, kEnvHold, e.hold); break;
    case kEnvHold: env_enter(v, kEnvDecay, e.decay); break;
    case kEnvDecay: env_enter(v, kEnvSustain, 0); break;
    case kEnvRelease: stop_voice(v); break;  // the release has run out
    default: break;
  }
}

// NoteOff / NoteFade / the stop. A voice with an envelope releases through it, over its own release time (a fade: 300 ms); in the delay the note has not
// started and ends at once; in the release already, the release starts again from where the level is
// (a second NoteOff, or a NoteFade after a NoteOff).
void VoicePool::release_voice(Voice& v, bool fade) noexcept {
  for (ModGen& g : v.mg) {
    g.key = false;  // the ADSRs' note is off (a NoteOff, a NoteFade and the stop all end it); an LFO runs on with the voice
  }
  if (v.env_stage == kEnvOff) {
    start_release(v, fade ? kFadeSamples : kReleaseSamples);
  } else if (v.env_stage == kEnvDelay) {
    stop_voice(v);
  } else {
    env_enter(v, kEnvRelease, fade ? kFadeEnvUnits : v.env->release);
  }
}

// ---- Rendering -----------------------------------------------------------------------------------------------------------

std::int16_t* VoicePool::touch(unsigned i) noexcept {
  std::int16_t* const block = blocks_[i];
  if ((touched_ & (1u << i)) == 0u) {
    touched_ |= 1u << i;
    const std::uint32_t zero = 0;
    for (std::uint32_t k = 0; k < kBlock; k += 2u) {
      std::memcpy(block + k, &zero, sizeof zero);  // one word store; a plain loop, not a memset call (the hot path calls nothing in flash)
    }
  }
  return block;
}

// OS-ODD11 slice 2: the closed form of run<Ramp, false> for a voice of mode 0 (a one shot or a forward loop), without the samples. The loop reads sample j at the position the last step left, wraps it when its
// integer part has reached `end` (idx = loop_start + (idx - loop_start) % loop_len, the fraction kept) and then steps; so after n samples the position is the wrap of sample n - 1's, plus one step, and a
// wrap is a subtraction of whole loops, which commutes with the steps: the position of sample n - 1 is phase + (n - 1) x inc, wrapped once. A one shot has run out when that sample's integer part is at
// `end` (the position only grows): the loop would have stopped at or before it. The amplitude steps and is held to [0, top] every sample: after the first step it is inside [0, top], and from a value inside,
// n - 1 more steps of one sign that stick at the ends are the one clamp of the sum, so two clamps are the loop's (whatever amp was).
std::uint32_t VoicePool::advance(Voice& v, std::uint32_t n, bool ramp) noexcept {
  std::uint64_t ph = v.phase + static_cast<std::uint64_t>(n - 1u) * v.inc;
  std::uint32_t idx = static_cast<std::uint32_t>(ph >> 32);
  if (idx >= v.end) {
    if (v.loop_len == 0u) {
      return 0;  // the one shot has played its last sample
    }
    const std::uint32_t loop_start = v.end - v.loop_len;
    idx = loop_start + (idx - loop_start) % v.loop_len;  // the one division, at a wrap (UDIV)
    ph = (static_cast<std::uint64_t>(idx) << 32) | static_cast<std::uint32_t>(ph);
  }
  v.phase = ph + v.inc;
  if (ramp) {
    const std::int64_t top = static_cast<std::int64_t>(v.gain > v.gain_prev ? v.gain : v.gain_prev) * 256;
    const std::int64_t step = v.amp_step;
    std::int64_t a = static_cast<std::int64_t>(v.amp) + step;
    a = a < 0 ? 0 : (a > top ? top : a);
    a += static_cast<std::int64_t>(n - 1u) * step;
    v.amp = static_cast<std::int32_t>(a < 0 ? 0 : (a > top ? top : a));
  }
  return n;
}

// One voice's span, in segments of constant envelope (see the declaration). Adv: the segments are the same, the sample loop is advance() and there is no block (`out` is not read, the voice holds no residual).
template <bool Adv>
O3OS_AUDIO_INLINE void VoicePool::render_seg(Voice& v, [[maybe_unused]] std::int16_t* out, std::uint32_t begin, std::uint32_t end) noexcept {
  std::uint32_t pos = begin;
  while (pos < end) {
    if (!v.playing) {
      if constexpr (!Adv) {
        add_residual(v, out + pos, end - pos);
      }
      return;
    }
    std::uint32_t n = end - pos;
    const bool env = v.env_stage != kEnvOff;
    bool ramp;
    bool stage_end = false;
    if (env) {
      stage_end = n >= v.env_left;  // this segment ends the stage (never in the sustain: env_left is kEnvForever)
      if (stage_end) {
        n = v.env_left;
      }
      ramp = env_segment(v, n);
    } else {
      ramp = v.ramp_left != 0u;
      if (ramp && n > v.ramp_left) {
        n = v.ramp_left;
      }
    }
    std::uint32_t made;
    if constexpr (Adv) {
      made = advance(v, n, ramp);
    } else if (v.mode != 0u) {  // a backward or ping-pong loop: the one loop that steps its amplitude whether it moves or not
      v.amp_step = ramp ? v.amp_step : 0;
      made = v.mode == kModeGranular ? run_gran(v, out + pos, n) : run_dir(v, out + pos, n);
    } else {
      made = ramp ? run_ramp(v, out + pos, n) : run_steady(v, out + pos, n);
    }
    if constexpr (!Adv) {
      add_residual(v, out + pos, made);
    }
    pos += made;
    if (made < n) {
      stop_voice(v);  // the one shot ended
    } else if (env) {
      env_advance(v, n, stage_end);
    } else if (ramp) {
      v.ramp_left -= made;
      if (v.ramp_left == 0u) {
        if (v.releasing) {
          stop_voice(v);  // the release has run out
        } else {
          v.amp = v.gain * 256;  // the attack is done
          v.amp_step = 0;
          v.gain_prev = v.gain;
        }
      }
    }
  }
}

void VoicePool::render_voice(Voice& v, std::int16_t* out, std::uint32_t begin, std::uint32_t end) noexcept { render_seg<false>(v, out, begin, end); }

// The stages that only make a voice sound, after its samples and its residual are in its block: the tremolo, before the filter, the filter over the whole span
// (the voice's samples, its residual and the silence after a one shot), the overdrive and the bit depth (OS-IFX1, in flash). Inlined into render and into odd_span: one text, two callers.
O3OS_AUDIO_INLINE void VoicePool::post_stages(Voice& v, std::int16_t* block, std::uint32_t begin, std::uint32_t end) noexcept {
  if ((v.mmask & kMmTrem) != 0u) {
    trem_span(v, block, begin, end - begin);  // OS-MOD
  }
  if (v.filt) {
    filter_run(v, v.fmod ? &dmod(v, 3u, samples_[v.slot].filter.mod) : nullptr, block, begin, end - begin);
  }
  if ((v.drive | v.crush) != 0u) {
    shape_span(v, block + begin, end - begin);
  }
}

void VoicePool::render(std::uint32_t begin, std::uint32_t end) noexcept {
  if (begin >= end || end > kBlock) {
    return;
  }
  const std::uint32_t gate_now = gate_;  // render context: set_gate is called between blocks
  for (unsigned i = 0; i < kMaxVoices; ++i) {
    Voice& v = v_[i];
    if (v.playing || v.resid != 0) {
      // OS-ODD11 (voices.h GATE): a playing voice on an inaudible strip keeps its note and skips what only makes it sound. (a residual tail is added as always; the block is not fed).
      // OS-ODD11 UTAIL: a block with nothing gated and no voice owing a re-entry (every unmuted block) costs one compare against a register and one byte load a voice a span; the rest is odd_span, in flash.
      // OS-ODD11 slice 2: a gated voice of the plain kind is advanced in closed form, with no block (gated_span, flash); the others go on below as in slice 1.
      const bool gate = gate_now != 0u && v.playing && ((gate_now >> v.track) & 1u) != 0u;  // (note_on takes a track below kVoiceTracks = 16 only)
      if (__builtin_expect(gate, 0) && gated_span(v, begin, end)) {
        continue;
      }
      const bool first = (touched_ & (kSeen << i)) == 0u;  // the voice's first span of this block (touched, or visited by gated_span)
      std::int16_t* const block = touch(i);
      if (v.mmask != 0u && v.playing && (first || v.mneed)) {
        mod_block(v, samples_[v.slot], begin);  // OS-MOD: the pan and the finetune, once a block (flash)
      }
      render_voice(v, block, begin, end);
      if (__builtin_expect(gate || v.dk != 0u, 0)) {
        odd_span(v, i, block, begin, end, gate);
        continue;
      }
      post_stages(v, block, begin, end);
    }
  }
  // The filter LFOs' tick count moves on by the span (after the voices: they read it at the span's start), held at the end of the step (see FILTER MODULATION).
  tick_q_ += static_cast<std::uint64_t>(end - begin) * tick_rate_q_;
  if (tick_q_ > kTickMaxQ24) {
    if (running_) {
      tick_q_ = kTickMaxQ24;
    } else {
      tick_carry_stopped(*this);  // flash, at most once a block (below)
    }
  }
}

// OS-BUG-WTLFO: no transport, no Step events: the count goes on into the next step (576 ticks, 1000 BPM at most is 14 ticks a block, so one carry is the whole of it). The first step's base is
// the same for tick_steps_ 0 and 1 (flfo_level), so from 0 the carry goes to 2. In flash (O3OS_AUDIO_COLD): taken at most once per block, once per step of the stopped clock, not per sample.
void VoicePool::tick_carry_stopped(VoicePool& pool) noexcept {
  pool.tick_q_ -= static_cast<std::uint64_t>(kTicksPerStep) << 24;
  pool.tick_steps_ = pool.tick_steps_ == 0u ? 2u : pool.tick_steps_ + 1u;
  pool.tick_q_ = pool.tick_q_ > kTickMaxQ24 ? kTickMaxQ24 : pool.tick_q_;
}

// ---- The filter (voices.h, FILTER; svf.h) --------------------------------------------------------------------------------------

// The voice's filter from the track's Cutoff FX when it has one (on whatever the instrument says, with the instrument's resonance, or Q 0.7 when its filter is off), else
// from the instrument: on with its type, cutoff and resonance, or off. The state is the caller's.
void VoicePool::filter_apply(Voice& v, const SampleDesc& s, const FilterFx& fx, unsigned lfo_fx) noexcept {
  const FilterDesc& fd = s.filter;
  v.fmod = fd.enable && (fd.mod.kind != kFmodNone || lfo_fx != 0u);  // the modulator (the Filter LFO FX is one) needs the instrument's own filter on, whatever the FX did
  v.mg[kMgCut].need = v.fmod;                     // a new base: the next sample re-evaluates
  v.filt = fx.perf_type != 0u || fx.on || fd.enable;
  if (v.filt) {
    // Instrument < Cutoff FX < performance layer (PERFORMANCE FILTER): the layer works on the FX's value, else the instrument's, and has the type of its slot. The resonance
    // is the instrument's, which a filter that only an FX turned on does not have (Q 0.7).
    float cutoff = fx.on ? fx.cutoff : fd.cutoff;
    std::uint8_t type = fx.on ? fx.type : fd.type;
    if (fx.perf_type != 0u) {
      cutoff += static_cast<float>(fx.perf_off) * 0.01f;
      cutoff = cutoff < 0.0f ? 0.0f : (cutoff > 1.0f ? 1.0f : cutoff);
      type = static_cast<std::uint8_t>(fx.perf_type - 1u);
    }
    v.ftype = type;
    v.fbase = cutoff;
    svf_set(v.svf, cutoff, fd.enable ? fd.resonance : 0.0f);
  }
  v.fcut = v.fbase;
}

// ---- The filter modulation (voices.h, FILTER MODULATION) -----------------------------------------------------------------------

void VoicePool::mod_start(ModGen& g, const FilterMod& m, std::uint32_t step, float kill, unsigned lfo_fx, unsigned rate0) noexcept {
  const bool lfo = lfo_fx != 0u || m.kind == kFmodLfo;  // the Filter LFO FX makes the modulator an LFO whatever the instrument has
  g.kind = lfo ? kFmodLfo : kFmodEnv;
  g.since = 0;
  g.need = true;
  if (!lfo) {
    g.stage = kFeDelay;
    g.e_t = 0.0f;
    g.key = true;
    g.e_kill = kill;
    g.e_max = kill;
    g.e_out = kill;
  } else {
    const std::uint32_t period = static_cast<std::uint32_t>(static_cast<float>(kTicksPerStep) * kLfoRate[lfo_fx != 0u ? lfo_fx - 1u : m.rate + rate0]);  // truncated in float: 1/3 step is 191
    g.l_period = period != 0u ? period : 1u;
    g.l_inv = 1.0f / static_cast<float>(g.l_period);  // the note's one division
    g.l_shift = (step % 36u) * kTicksPerStep;
    g.l_q = 0xFFFFFFFFu;  // the random shape draws its first level at the first evaluation
    g.l_rand = 0.0f;
  }
}

// The ADSR after the samples since its last evaluation: the stage changes it owes (the remainder of a stage carries over), then the level of the stage it is in.
float VoicePool::fenv_eval(ModGen& g, const FilterMod& m) noexcept {
  if (g.stage == kFeSustain && g.key) {
    // Held in the sustain (the common steady state): the general path below ends with exactly these writes, without building the stage table.
    const float level = clampf(m.sustain * m.amount, -1.0f, 1.0f);
    g.since = 0;
    g.e_max = level;
    g.e_t = 0.0f;
    g.e_out = level;
    return level;
  }
  float t = g.e_t + static_cast<float>(g.since);
  g.since = 0;
  std::uint8_t stage = g.stage;
  const bool key = g.key;
  // Each stage's length in ms, by FStage. A table, not a lambda: GCC outlined the lambda into flash, and the hot path must stay in SRAM_L
  // (tools/check_ramfunc.py).
  const std::uint16_t stage_ms[] = {0u, m.delay, m.attack, m.hold, m.decay, 0u, m.release};
  static_assert(kFeRelease == 6u, "stage_ms is indexed by FStage");
  for (unsigned guard = 0; guard < 8u && stage != kFeOff; ++guard) {  // delay, attack, hold, decay, sustain, release, off: seven changes at the most
    if (!key && stage != kFeRelease) {
      // the note is off: a NoteOff in the delay ends the modulation; any other stage (the sustain too) starts the release from the level it was at
      stage = stage == kFeDelay ? kFeOff : kFeRelease;
      t = 0.0f;
      continue;
    }
    if (stage == kFeSustain) {
      break;  // held while the note is
    }
    const float len = static_cast<float>(stage <= kFeRelease ? stage_ms[stage] : 0u) * kSamplesPerMs;
    if (t < len) {
      break;
    }
    t -= len;
    stage = stage == kFeRelease ? static_cast<std::uint8_t>(kFeOff) : static_cast<std::uint8_t>(stage + 1u);
  }
  const float amount = m.amount;
  const float sustain = m.sustain;
  const float len = static_cast<float>(stage <= kFeRelease ? stage_ms[stage] : 0u) * kSamplesPerMs;
  float temp = 0.0f;
  switch (stage) {
    case kFeDelay:
      temp = g.e_kill;
      g.e_max = temp;
      break;
    case kFeAttack:  // linear from the kill level to amount, never below the kill level, never past 1
      temp = len > 0.0f ? g.e_kill + (t / len) * (amount - g.e_kill) : amount;
      temp = clampf(temp, g.e_kill, 1.0f);
      g.e_max = temp;
      break;
    case kFeHold:
      temp = amount;
      g.e_max = temp;
      break;
    case kFeDecay:
      temp = len > 0.0f ? ((1.0f - t / len) * (1.0f - sustain) + sustain) * amount : sustain * amount;
      temp = clampf(temp, -1.0f, 1.0f);
      g.e_max = temp;
      break;
    case kFeSustain:
      temp = clampf(sustain * amount, -1.0f, 1.0f);
      g.e_max = temp;
      t = 0.0f;
      break;
    case kFeRelease:
      if (g.e_max == 0.0f && sustain != 0.0f && amount != 0.0f) {
        g.e_max = sustain * amount;  // guard for a release entered with no level yet
      }
      temp = len > 0.0f ? clampf((1.0f - t / len) * g.e_max, -1.0f, 1.0f) : 0.0f;
      break;
    default:
      g.e_max = 0.0f;
      t = 0.0f;
      break;
  }
  g.stage = stage;
  g.e_t = t;
  g.e_out = clampf(temp, -1.0f, 1.0f);
  return g.e_out;
}

// The LFO's level at the transport's tick count `samples_on` samples into the span: A x the shape of the phase u in the period.
float VoicePool::flfo_level(ModGen& g, const FilterMod& m, std::uint32_t samples_on) noexcept {
  std::uint64_t tq = tick_q_ + static_cast<std::uint64_t>(samples_on) * tick_rate_q_;
  tq = (running_ && tq > kTickMaxQ24) ? kTickMaxQ24 : tq;  // held at the end of a step only while the transport runs (stopped: it runs on, linear in tq; render() carries it)
  const std::uint32_t x = (tick_steps_ != 0u ? tick_steps_ - 1u : 0u) * kTicksPerStep + static_cast<std::uint32_t>(tq >> 24) + g.l_shift;
  const std::uint32_t q = x / g.l_period;  // the one division of an evaluation (UDIV)
  const float u = static_cast<float>(x - q * g.l_period) * g.l_inv;
  const float a = m.lfo_amount;
  float temp;
  switch (g.l_ovr != 0u ? g.l_ovr - 1u : static_cast<unsigned>(m.shape)) {  // CF2: the tracker FX's waveform, else the instrument's
    case 0: temp = clampf((1.0f - u) * a, -1.0f, 1.0f); break;  // reverse saw: the decay alone
    case 1: temp = clampf(u * a, 0.0f, 1.0f); break;            // saw: the attack alone
    case 2: temp = u < 0.5f ? clampf(2.0f * u * a, 0.0f, 1.0f) : clampf((2.0f - 2.0f * u) * a, -1.0f, 1.0f); break;  // triangle: attack then decay, half a period each
    case 3: temp = u < 0.5f ? a : 0.0f; break;                  // square: the hold at amount, then the release from 0
    case kLfoShapeSine: temp = lfo_sine_level(u) * a; break;    // sine (OS-LFOSINE): (1 - cos 2 pi u) / 2, from 0 up to 1 at u = 1/2, the triangle's phase; a quintic, no libm (lfo_sine.h)
    default:                                                   // random: a level in [0, A) held for a period
      if (q != g.l_q) {
        g.l_q = q;
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        const std::uint32_t n = a > 0.0f ? static_cast<std::uint32_t>(a * 1000.0f) : 0u;  // a random number in 0 .. amount, in thousandths
        g.l_rand = n != 0u ? static_cast<float>(rng_ % n) * 0.001f : 0.0f;
      }
      temp = g.l_rand;
      break;
  }
  return clampf(temp, -1.0f, 1.0f);
}

float VoicePool::flfo_eval(ModGen& g, const FilterMod& m, std::uint32_t samples_on) noexcept {
  return 2.0f * flfo_level(g, m, samples_on) - m.lfo_amount;
}

// The voice's filter over the n samples of `block` from `pos`, in place (OS-CPUOPT1, C3: one function for the plain filter and the modulated one, so that the three type loops exist once and the modulated
// span's 16-sample chunks run inside it, the filter state staying in registers from one chunk to the next). `m` null: the whole span is one chunk. `m` set: the cutoff is re-evaluated at every 16th
// sample of the block (and at the first of a note), by the modulator the instrument has on it, and svf_set_cutoff (which keeps the coefficient while the table index is the same) puts it in.
void VoicePool::filter_run(Voice& v, const FilterMod* m, std::int16_t* block, std::uint32_t pos, std::uint32_t n) noexcept {
  ModGen& g = v.mg[kMgCut];
  std::uint32_t done = 0;
  while (done < n) {
    const std::uint32_t at = pos + done;
    const std::uint32_t left = n - done;
    std::uint32_t c = left;
    if (m != nullptr) {
      const std::uint32_t phase = at & (kFilterCtl - 1u);
      c = kFilterCtl - phase < left ? kFilterCtl - phase : left;  // up to the next 16th of the block, or the span's end
      if (phase == 0u || g.need) {
        g.need = false;
        const float mod = g.kind == kFmodEnv ? fenv_eval(g, *m) : flfo_eval(g, *m, done);
        const float sum = v.fbase + mod;
        v.fcut = !(sum >= 0.0f) ? 0.0f : (sum > 1.0f ? 1.0f : sum);  // base + mod held to 0..1 (a NaN is the low end, as svf_set_cutoff takes it)
        svf_set_cutoff(v.svf, v.fcut);
      }
    }
    switch (v.ftype) {
      case kFilterHigh: svf_run<kFilterHigh>(v.svf, block + at, c); break;
      case kFilterBand: svf_run<kFilterBand>(v.svf, block + at, c); break;
      default: svf_run<kFilterLow>(v.svf, block + at, c); break;
    }
    g.since = static_cast<std::uint16_t>(g.since + (m != nullptr ? c : 0u));
    done += c;
  }
}

// OS-MOD (voices.h AMP LFO MODULATION): the tremolo over n samples that start at index `pos` of the block: the multiply only, the multipliers it walks to come from mod_block (v.tgt, one for each
// kFilterCtl-th index of the block, v.t0 for a note that starts inside a grid cell). The multiplier (Q23) is walked linearly between the grid points, so that it does not depend on how events split
// the block. Applied to the voice's whole span, the declick residual included, ahead of the filter. An LFO that was switched off live ramps back to unity and then ends the pass.
void VoicePool::trem_span(Voice& v, std::int16_t* block, std::uint32_t pos, std::uint32_t n) noexcept {
  ModGen& g = v.mg[kMgTrem];
  std::int32_t cur = v.tcur;
  std::int32_t step = v.tstep;
  std::uint32_t done = 0;
  while (done < n) {
    const std::uint32_t at = pos + done;
    const std::uint32_t phase = at & (kFilterCtl - 1u);
    const std::uint32_t left = n - done;
    const std::uint32_t c = kFilterCtl - phase < left ? kFilterCtl - phase : left;
    if (phase == 0u || g.need) {
      const std::int32_t target = phase == 0u ? v.tgt[at / kFilterCtl] : v.t0;
      if (g.need) {
        cur = target;  // the first sample of the note: no ramp
        step = 0;
        g.need = false;
      } else {
        step = (target - cur) >> 4;  // to be there at the next grid point, 16 samples on
      }
    }
    std::int16_t* const out = block + at;
    for (std::uint32_t i = 0; i < c; ++i) {
      cur += step;
      out[i] = static_cast<std::int16_t>((static_cast<std::int32_t>(out[i]) * (cur >> 8)) >> 15);  // |out| <= 32768, cur >> 8 <= 32768: 2^30 at the most
    }
    cur = cur < 0 ? 0 : (cur > kTremOne ? kTremOne : cur);
    done += c;
  }
  if ((v.mmask & kMmTremOff) != 0u && cur > kTremOne - 64) {
    cur = kTremOne;
    step = 0;
    v.mmask = static_cast<std::uint8_t>(v.mmask & ~(kMmTrem | kMmTremOff));  // back at unity: the pass is over
  }
  v.tcur = cur;
  v.tstep = step;
}

// The track's filter state in one place (voices.h CUTOFF FX and PERFORMANCE FILTER): the sequencer's Cutoff FX (on or off, its type 0..2 and value 0..100) and the performance
// layer's hold (0: none, else type + 1 in 1..3, and its offset -100..100). The voice sounding on the track takes it at once; a filter that was out of the path starts from rest.
// `track` kVoiceTracks: the Cutoff FX of every track ends (the layer's hold stays). Not hot: it runs in flash, through VoicePool::filter_set_ (voices.h set_filter), and the arguments
// are in range (Engine::dispatch decodes them from sanitised events; the wrappers in voices.h check theirs).
void VoicePool::filter_set_impl(VoicePool& pool, unsigned track, bool fx_on, unsigned fx_type, unsigned fx_value, unsigned perf_type_plus_one, int perf_offset) noexcept {
  const bool all = track == kVoiceTracks;
  for (unsigned t = all ? 0u : track; t < (all ? kVoiceTracks : track + 1u) && t < kVoiceTracks; ++t) {
    FilterFx& fx = pool.fx_filter_[t];
    fx.on = fx_on;
    if (!all) {
      fx.type = static_cast<std::uint8_t>(fx_type);
      fx.cutoff = static_cast<float>(fx_value) / 100.0f;  // (value - min) / (max - min) of the band-pass FX's range, 0..100
      fx.perf_type = static_cast<std::uint8_t>(perf_type_plus_one);
      fx.perf_off = static_cast<std::int8_t>(perf_offset);
    }
    const unsigned i = pool.find(t);
    if (i != kNone && pool.v_[i].playing) {
      Voice& v = pool.v_[i];
      const bool was = v.filt;
      filter_apply(v, pool.samples_[v.slot], fx, pool.fx_lfo_[t]);
      if (!was && v.filt) {
        svf_reset(v.svf);
      }
      (void)pool.perf_dest(v, 3u, true);  // OS-PERF-LFO: the layer's FLT LFO goes on over the new base (or starts, when a held filter has just given it one to move)
    }
  }
}

// The Filter LFO FX of one track, or of every track (`track` kVoiceTracks: all end; `on` is then false). Not hot: it runs in flash, through VoicePool::lfo_set_ (voices.h
// set_filter_lfo). The voice sounding on the track takes it at once (FILTER LFO FX): the LFO starts from the cursor's step, or, when an LFO already runs, only its rate changes.
void VoicePool::lfo_set_impl(VoicePool& pool, unsigned track, bool on, unsigned rate) noexcept {
  const bool all = track == kVoiceTracks;
  for (unsigned t = all ? 0u : track; t < (all ? kVoiceTracks : track + 1u) && t < kVoiceTracks; ++t) {
    pool.fx_lfo_[t] = on ? static_cast<std::uint8_t>((rate >= kLfoRates ? kLfoRates - 1u : rate) + 1u) : std::uint8_t{0};
    const unsigned i = pool.find(t);
    if (all) {
      // OS-PERF-LFO-STEP: FxReset also ends the step FX 23, 24, 33 and 36 (rows 0..3 -> destinations 0, 1, 2, 4); the sounding voice takes the end at once. Before the `continue` below.
      for (unsigned row = 0; row < 4u; ++row) {
        if (pool.step_lfo_[row][t] != 0u) {
          pool.step_lfo_[row][t] = 0;
          if (i != kNone && pool.v_[i].playing) {
            pool.perf_sync(pool.v_[i], row == 3u ? 4u : row);
          }
        }
      }
    }
    if (i == kNone || !pool.v_[i].playing) {
      continue;
    }
    Voice& v = pool.v_[i];
    ModGen& g = v.mg[kMgCut];
    const SampleDesc& s = pool.samples_[v.slot];
    const unsigned fx = pool.fx_lfo_[t];
    const bool was_lfo = v.fmod && g.kind == kFmodLfo;
    filter_apply(v, s, pool.fx_filter_[t], fx);  // the base, and whether anything modulates it now
    if (pool.perf_dest(v, 3u, true)) {
      continue;  // OS-PERF-LFO: the layer's FLT LFO has the cutoff modulator (over the FX 35's and the instrument's)
    }
    if (v.fmod) {
      if (was_lfo && (fx != 0u || s.filter.mod.kind == kFmodLfo)) {
        const unsigned r = fx != 0u ? fx - 1u : s.filter.mod.rate;  // an LFO runs and an LFO is wanted: its rate moves, its phase is the transport's
        const std::uint32_t period = static_cast<std::uint32_t>(static_cast<float>(kTicksPerStep) * kLfoRate[r]);
        g.l_period = period != 0u ? period : 1u;
        g.l_inv = 1.0f / static_cast<float>(g.l_period);
      } else {
        mod_start(g, s.filter.mod, pool.cursor_step_, 0.0f, fx);
      }
    } else {
      g.kind = kFmodNone;
    }
  }
}

// The transport start (restart_clock, voices.h LFOS WHILE STOPPED). Not hot: a rare event, in flash through VoicePool::clock_restart_. The count starts over. When the transport was STOPPED, the
// count had gone on at the tempo for the voices that sound, and their LFOs keep the phase they are at: the voice's shift is made the phase it has now (x mod period, which the restarted count
// then adds its ticks to), and a random level already drawn is held (its period index is 0 from here). A restart while the transport ran (a Play while playing) is what it was: the phase of
// the new count.
void VoicePool::clock_restart_impl(VoicePool& pool) noexcept {
  if (!pool.running_) {
    const std::uint32_t base = pool.tick_base();
    for (Voice& v : pool.v_) {
      if (!v.playing) {
        continue;
      }
      for (ModGen& g : v.mg) {
        if (g.kind != kFmodLfo) {
          continue;
        }
        g.l_shift = (base + g.l_shift) % g.l_period;
        g.l_q = g.l_q == 0xFFFFFFFFu ? g.l_q : 0u;
      }
    }
  }
  pool.tick_steps_ = 0;
  pool.tick_q_ = 0;
  pool.cursor_step_ = 0;
  pool.running_ = true;
}

// The first Step after a stop (on_step with the count run on while stopped, LFOS WHILE STOPPED): the count snaps to the new step (tick_steps_ + 1, tick_q_ 0), which would move the phase of every LFO
// that sounds by up to 575 ticks. Each voice's shift takes the difference (x mod period, the same phase at the new count), and a random level already drawn is held (its period index is the new
// count's). Flash, once per resume.
void VoicePool::resume_phase(VoicePool& pool) noexcept {
  const std::uint32_t old_base = pool.tick_base();
  const std::uint32_t new_base = pool.tick_steps_ * kTicksPerStep;  // (tick_steps_ + 1) - 1 steps, tick_q_ 0
  for (Voice& v : pool.v_) {
    if (!v.playing) {
      continue;
    }
    for (ModGen& g : v.mg) {
      if (g.kind != kFmodLfo) {
        continue;
      }
      const std::uint32_t p = g.l_period;
      g.l_shift = ((old_base + g.l_shift) % p + p - new_base % p) % p;
      g.l_q = g.l_q == 0xFFFFFFFFu ? g.l_q : (new_base + g.l_shift) / p;
    }
  }
}

void VoicePool::on_step(unsigned step) noexcept {
  if (!running_) {
    resume_phase(*this);
  }
  ++tick_steps_;
  tick_q_ = 0;
  cursor_step_ = step;
  running_ = true;
}

void VoicePool::end_block() noexcept {
  for (unsigned t = 0; t < kVoiceTracks; ++t) {
    feed_[t] = nullptr;
  }
  const std::uint32_t fed = touched_ & ~static_cast<std::uint32_t>(gated_);  // OS-ODD11: a voice that skipped its post stages is not fed (the strip is closed or just opening)
  stats_.gated_blocks += gated_ != 0u ? 1u : 0u;
  std::uint32_t in_use = 0;  // the voices' load (voices.h, voice_snapshot)
  for (unsigned i = 0; i < kMaxVoices; ++i) {
    if ((fed & (1u << i)) != 0u && v_[i].track < kVoiceTracks) {
      feed_[v_[i].track] = blocks_[i];
    }
    in_use += (v_[i].playing || v_[i].resid != 0) ? 1u : 0u;
  }
  pub_in_use_.store(in_use, std::memory_order_relaxed);
  if (in_use > pub_peak_.load(std::memory_order_relaxed)) {
    pub_peak_.store(in_use, std::memory_order_relaxed);
  }
  pub_steals_.store(stats_.steals, std::memory_order_relaxed);
  std::uint32_t live = 0;  // the live voice's position (voices.h, live_position)
  if (live_idx_ < kMaxVoices && v_[live_idx_].playing && v_[live_idx_].age == live_age_) {
    const std::uint64_t pos = v_[live_idx_].phase >> 32;
    live = kLivePlaying | ((static_cast<std::uint32_t>(v_[live_idx_].slot) + 1u) << 24) | (pos > 0xFFFFFFu ? 0xFFFFFFu : static_cast<std::uint32_t>(pos));
  }
  live_pos_.store(live, std::memory_order_relaxed);
}

// ---- Notes ---------------------------------------------------------------------------------------------------------------

unsigned VoicePool::find(unsigned track) const noexcept {
  for (unsigned i = 0; i < kMaxVoices; ++i) {
    if ((v_[i].playing || v_[i].resid != 0) && v_[i].track == track) {
      return i;
    }
  }
  return kNone;
}

// A free voice; else one that only holds a click's tail; else the oldest playing one.
unsigned VoicePool::allocate() noexcept {
  unsigned fading = kNone;
  unsigned oldest = kNone;
  for (unsigned i = 0; i < kMaxVoices; ++i) {
    const Voice& v = v_[i];
    if (v.playing) {
      if (oldest == kNone || static_cast<std::int32_t>(v.age - v_[oldest].age) < 0) {
        oldest = i;
      }
    } else if (v.resid != 0) {
      fading = fading == kNone ? i : fading;
    } else {
      return i;
    }
  }
  return fading != kNone ? fading : oldest;
}

bool VoicePool::note_on(unsigned track, unsigned note, unsigned slot, unsigned velocity) noexcept {
  if (track >= kVoiceTracks || slot >= kDescSlots || samples_[slot].length == 0u) {
    ++stats_.ignored;
    return false;
  }
  const SampleDesc& s = samples_[slot];
  // The part of the sample this note plays and the note its pitch follows. A beat slice plays the slice the note names at the root's pitch (note 60,
  // "zero pitch"); a slice that has no length, or starts past the end, is silent. The boundaries come from the arena: clamped, so
  // that no read leaves [0, length) whatever they hold.
  const std::int16_t* data = s.data;
  std::uint32_t length = s.length;
  unsigned pitch_note = note;
  bool loops = s.loop_end - s.loop_start >= 2u;  // set_sample made it valid or zeroed it
  if (s.beat_slice) {
    pitch_note = kRootNote;
    loops = false;
    std::uint32_t from = 0, span = s.length;
    if (!beat_slice_span(s, note, forced_slice_[track], from, span)) {
      cut(track);  // the note replaces the track's, and a slice with no length plays nothing
      ++stats_.ignored;
      return false;
    }
    data += from;
    length = span;
  }
  // OS-PERFFX P7 (voices.h SMP POS AND SMP END): the layer's first and last frame of the part, made in flash (smp_window, only when the layer holds one of the two on this track: note_on itself runs from
  // SRAM_L). A start at or past the end plays nothing (the beat slice's rule).
  std::uint32_t first = 0;
  if ((((perf_.on[kPerfPosSlot] | perf_.on[kPerfEndSlot]) >> track) & 1u) != 0u) {
    const std::uint64_t w = smp_window(track, s, loops, length);
    if (w == kSmpSilent) {
      return false;
    }
    first = static_cast<std::uint32_t>(w >> 32);
    length = static_cast<std::uint32_t>(w);
  }
  unsigned i = find(track);
  const bool retrigger = i != kNone;
  if (!retrigger) {
    i = allocate();
    if (v_[i].playing) {
      ++stats_.steals;
    }
  }
  Voice& v = v_[i];
  last_idx_ = static_cast<std::uint8_t>(i);
  if (retrigger) {
    carry(v, v.playing ? v.last : 0);  // the note that was sounding fades out under the new one
  } else {
    v.resid = 0;  // another track's tail (or nothing) is not this track's to carry
    v.resid_dec = 0;
  }
  // The gain: the instrument's own (Q16, from its volume through the project's law), or a step's velocity, linear and replacing it (voices.h, LEVEL).
  std::int32_t gain = s.gain;
  const bool bvel = velocity != kVelocityDefault;
  if (bvel) {
    const unsigned vel = velocity > 127u ? 127u : velocity;
    gain = static_cast<std::int32_t>((vel * 65536u + 63u) / 127u);  // 127 is exactly unity
  }
  v.data = data;
  v.phase = static_cast<std::uint64_t>(first) << 32;  // 0, or the layer's SMP POS (OS-PERFFX P7)
  v.inc = pitch_inc(pitch_note, s, s.fine_tune, 0);  // OS-RATEPITCH: in flash
  v.rp_ok = false;  // (OS-CPUOPT1: inc was not made by repitch)
  v.end = loops ? s.loop_end : length;
  v.loop_len = loops ? s.loop_end - s.loop_start : 0u;
  const AmpEnvelope* const env = s.env.enable ? &s.env : nullptr;
  v.base = gain;
  v.bvel = bvel;
  v.tlaw = !bvel;  // OS-COREFX CF2: the level law of the note, kept for the tracker FX (tracker_set turns bvel on)
  v.tvol = 255;
  v.pnote = static_cast<std::uint8_t>(pitch_note);
  gain = with_amount(gain, env);  // amount <= 1.0 (set_sample): never past kMaxGain
  v.gain = gain;
  v.gain_prev = gain;
  v.amp = 0;
  v.env = env;
  v.mode = loops ? s.loop_mode : 0u;
  v.bwd = false;
  if (s.granular) {
    grain_start(v, s);  // OS-GRAN (flash, once a note)
  }
  const bool filtered_before = retrigger && v.filt;
  ModGen& g = v.mg[kMgCut];
  const float kill = (retrigger && g.kind == kFmodEnv) ? g.e_out : 0.0f;  // the cutoff ADSR starts from where the old note's was
  filter_apply(v, s, fx_filter_[track], fx_lfo_[track]);
  if (!(filtered_before && v.filt)) {
    svf_reset(v.svf);  // the state runs on only across a retrigger that filters on both sides
  }
  if (v.fmod) {
    mod_start(g, s.filter.mod, cursor_step_, kill, fx_lfo_[track]);
  } else {
    g.kind = kFmodNone;
  }
  v.slot = static_cast<std::uint8_t>(slot);
  v.track = static_cast<std::uint8_t>(track);  // before mods_start: slide_start reads it for the layer's TUNE (a voice reused from another track would carry that track's)
  mods_start(v, s, retrigger, track, i);  // OS-MOD: the pan, finetune and tremolo modulators of the instrument, if it has any, and (last, OS-PERF-CPU3) the rest of the note's start: the envelope or attack ramp, the gate bit, age, playing (flash, once a note)
  return true;
}

void VoicePool::note_off(unsigned track) noexcept {
  const unsigned i = find(track);
  if (i != kNone) {
    release_voice(v_[i], false);
  }
}

void VoicePool::fade(unsigned track) noexcept {
  const unsigned i = find(track);
  if (i != kNone) {
    release_voice(v_[i], true);
  }
}

void VoicePool::cut(unsigned track) noexcept {
  const unsigned i = find(track);
  if (i != kNone && v_[i].playing) {
    stop_voice(v_[i]);
    pan_dirty_ &= ~(1u << track);  // OS-MOD: a pan this voice evaluated earlier in the block is not the track's now (the note that cut it, or the sampler's silence, gives it)
  }
}

void VoicePool::release_all() noexcept {
  for (unsigned i = 0; i < kMaxVoices; ++i) {
    if (v_[i].playing) {
      release_voice(v_[i], false);
    }
  }
}


// ---- Live instrument parameters (voices.h, LIVE PARAMETERS) ------------------------------------------------------------------------
// Everything below runs in flash, once per ParamChange (Engine::instrument_hook), never per sample.

std::uint16_t env_units(std::uint16_t ms) noexcept {
  const std::uint64_t samples = core::samples_in_ms(ms);  // ms at the engine's rate, truncated (core/audio_clock.h)
  const std::uint64_t units = (samples + 7u) >> 3;
  return static_cast<std::uint16_t>(units < 1u ? 1u : (units > 65535u ? 65535u : units));
}

namespace {
// OS-LFOLIVE: a modulation slot's SOURCE (0 Off, 1 Envelope, 2 LFO: the enable and loop bytes together, as model::EnvState reads them) and the LFO's shape, rate and amount, onto the descriptor, as
// make_sample_desc builds them from the stored fields (put_mod and the amp slot): equal field for field, so that a rebuild after the live edits is what the voices already have. Pure; the voices'
// side is VoicePool::apply_param. The amount is shared with the ADSR's in the descriptor (FilterMod::amount): switching to the LFO makes it the LFO's (kept in lfo_amount), and the sender
// puts the ADSR's back with an EnvAmount after a switch to Envelope (the descriptor holds one of the two).
InstApply apply_lfo_field(SampleDesc& d, unsigned e, unsigned f, float value) noexcept {
  FilterMod* mp = nullptr;
  switch (e) {
    case 0u: mp = &d.amp_mod; break;
    case 1u: mp = &d.pan_mod; break;
    case 2u: mp = &d.filter.mod; break;
    case 3u: mp = (d.wt_size != 0u && d.wt_windows > 1u) ? &d.pos_mod : nullptr; break;  // as the envelope fields: the wavetable position's slot belongs to a wavetable ...
    case 4u: mp = d.granular ? &d.pos_mod : nullptr; break;                              // ... and the granular position's to a granular instrument
    case 5u: mp = &d.tune_mod; break;
    default: return InstApply::Unknown;
  }
  if (mp == nullptr) {
    return InstApply::Ignored;
  }
  FilterMod& m = *mp;
  switch (f) {
    case 0u: {  // SOURCE
      const unsigned src = static_cast<unsigned>(whole(value, 0, 2));
      m.lfo = src == 2u;
      if (e == 0u) {  // the amp slot: an LFO is the tremolo (the plain gain), an ADSR the amp envelope; its modulator is never an ADSR
        d.env.lfo = m.lfo;
        d.env.enable = src == 1u;
        m.kind = m.lfo ? kFmodLfo : kFmodNone;
      } else {
        m.kind = src == 0u ? kFmodNone : (m.lfo ? kFmodLfo : kFmodEnv);
      }
      if (m.lfo || e == 0u) {  // (the amp slot's modulator is only ever an LFO: its amount is the LFO's whatever the source, as make_sample_desc has it)
        m.amount = m.lfo_amount;
      }
      break;
    }
    case 1u: m.shape = value > static_cast<float>(kLfoShapeSine) ? std::uint8_t{2} : static_cast<std::uint8_t>(whole(value, 0, kLfoShapeSine)); break;  // (a larger shape is a triangle, sanitize_mod)
    case 2u: m.rate = static_cast<std::uint8_t>(whole(value, 0, static_cast<int>(e == 0u ? kAmpLfoRates : kLfoRates) - 1)); break;
    default:
      m.lfo_amount = mod_amount(value);
      m.amount = (m.lfo || e == 0u) ? m.lfo_amount : m.amount;
      break;
  }
  return InstApply::Applied;
}
}  // namespace

InstApply apply_instrument_param(SampleDesc& d, unsigned field, float value, LevelLaw law) noexcept {
  using namespace inst_param;
  switch (field) {
    case Volume: {
      const unsigned volume = static_cast<unsigned>(whole(value, 0, 100));
      d.gain = level_gain_q16(volume, law);  // Q16, up to +24 dB under the 1.2+ law (the voices saturate); 50 is unity under it, 100 under the Og1114 law
      d.gain = d.gain > kernel::kMaxGain ? kernel::kMaxGain : d.gain;
      // the Amiga engine's volume 0..64 is the gain read against unity, min(64, round(64 x gain / 65536)): through the law, so an import (volume 64 is level 50, unity) reads the volume it was written at
      // (OS-BUG-PASTE24; it was volume * 64 / 100, a linear percent). Every level from the law's unity up reads 64.
      const std::uint32_t amiga = (static_cast<std::uint32_t>(d.gain) * 64u + 32768u) >> 16;  // gain <= kMaxGain: no overflow
      d.amiga_vol = static_cast<std::uint8_t>(amiga > 64u ? 64u : amiga);
      return InstApply::Applied;
    }
    case Panning:
      d.pan = static_cast<std::uint8_t>(whole(value, 0, 100));
      return InstApply::Applied;
    case DelaySend:
      d.delay_send = static_cast<std::uint8_t>(whole(value, 0, 100));
      return InstApply::Applied;
    case ReverbSend:
      d.reverb_send = static_cast<std::uint8_t>(whole(value, 0, 100));
      return InstApply::Applied;
    case Tune:
      d.tune = static_cast<std::int8_t>(whole(value, -24, 24));
      return InstApply::Applied;
    case FineTune:
      d.fine_tune = static_cast<std::int8_t>(whole(value, -100, 100));
      return InstApply::Applied;
    case FilterEnable:
      d.filter.wanted = value != 0.0f;
      d.filter.enable = d.filter.wanted && d.filter.type <= kFilterBand;
      return InstApply::Applied;
    case FilterType:
      d.filter.type = static_cast<std::uint8_t>(whole(value, 0, 255));
      d.filter.enable = d.filter.wanted && d.filter.type <= kFilterBand;
      return InstApply::Applied;
    case Cutoff:
      d.filter.cutoff = value;
      return InstApply::Applied;
    case Resonance:
      d.filter.resonance = value;
      return InstApply::Applied;
    case Glide:  // OS-IFX1: the descriptor holds them; the voices read them at a note (the glide) or at the next block (the overdrive and the bit depth)
      d.glide = static_cast<std::uint16_t>(whole(value, 0, 15000));
      return InstApply::Applied;
    case Overdrive:
      d.overdrive = static_cast<std::uint8_t>(whole(value, 0, 100));
      return InstApply::Applied;
    case BitDepth:
      d.bit_depth = static_cast<std::uint8_t>(whole(value, 4, 16));
      return InstApply::Applied;
    case PlayEngine:
      return InstApply::Ignored;
    default:
      break;
  }
  if (is_lfo_field(field)) {  // OS-LFOLIVE: a slot's SOURCE and LFO fields
    return apply_lfo_field(d, (field - kLfoFirst) / kLfoStride, (field - kLfoFirst) % kLfoStride, value);
  }
  if (field < kEnvelopeFirst) {
    return InstApply::Unknown;
  }
  const unsigned e = (field - kEnvelopeFirst) / kEnvelopeStride;
  const unsigned f = (field - kEnvelopeFirst) % kEnvelopeStride;
  if (e >= kEnvelopes || f >= kEnvelopeFields) {
    return InstApply::Unknown;
  }
  const std::uint16_t ms = static_cast<std::uint16_t>(whole(value, 0, 65535));
  if (e == 0u) {  // the amp envelope
    AmpEnvelope& a = d.env;
    switch (f) {
      case EnvEnable:  // an LFO slot keeps the plain gain (voices.h AMP ENVELOPE) and is the tremolo (AMP LFO MODULATION)
        a.enable = value != 0.0f && !a.lfo;
        d.amp_mod.kind = value != 0.0f && a.lfo ? kFmodLfo : kFmodNone;
        break;
      case EnvAmount: a.amount = static_cast<std::int32_t>(clamp01(value) * 65536.0f + 0.5f); break;
      case EnvAttack: a.attack = env_units(ms); break;
      case EnvDecay: a.decay = env_units(ms); break;
      case EnvSustain: a.sustain = static_cast<std::uint32_t>(clamp01(value) * 1073741823.0f); break;  // level x 1073741823.0; 1.0 is kEnvOne
      default: a.release = env_units(ms); break;
    }
    return InstApply::Applied;
  }
  FilterMod* mp = nullptr;  // a modulator's slot: the cutoff's (2), the pan's (1), the finetune's (5), and OS-GRAN's wavetable (3) and granular (4) position: an ADSR's fields, as stored; an LFO's own are the instrument's lfo[e]
  switch (e) {
    case 1u: mp = &d.pan_mod; break;
    case 2u: mp = &d.filter.mod; break;
    case 3u: mp = (d.wt_size != 0u && d.wt_windows > 1u) ? &d.pos_mod : nullptr; break;  // the wavetable position envelope runs for a wavetable only
    case 4u: mp = d.granular ? &d.pos_mod : nullptr; break;                              // ... and the granular position's for a granular instrument only
    case 5u: mp = &d.tune_mod; break;
    default: break;
  }
  if (e == 3u || e == 4u) {
    if (mp == nullptr) {
      return InstApply::Ignored;
    }
  }
  if (mp != nullptr) {
    FilterMod& m = *mp;
    switch (f) {
      case EnvEnable: m.kind = value == 0.0f ? kFmodNone : (m.lfo ? kFmodLfo : kFmodEnv); break;
      case EnvAmount: m.amount = m.lfo ? m.amount : mod_amount(value); break;
      case EnvAttack: m.attack = ms; break;
      case EnvDecay: m.decay = ms; break;
      case EnvSustain: m.sustain = mod_sustain(value); break;
      default: m.release = ms; break;
    }
    return InstApply::Applied;
  }
  return InstApply::Ignored;
}

// The voice's gain target from the descriptor (or the note's velocity) and the envelope's amount, now. A voice with an envelope ramps across its next segment (env_segment, from gain_prev);
// one without ramps over kDeclickSamples with the ramp of its attack (a gain that goes down keeps `gain_prev` at the old level so that run() does not cap it); a voice that is already in
// its release keeps the level it releases from.
void VoicePool::regain(Voice& v, const SampleDesc& s) noexcept {
  v.base = v.bvel ? v.base : s.gain;
  const std::int32_t g = with_amount(v.base, v.env);
  if (g == v.gain) {
    return;
  }
  if (v.env_stage != kEnvOff) {
    v.gain = g;
    return;
  }
  if (v.releasing) {
    return;
  }
  const std::int32_t now = (v.amp + 255) >> 8;  // the level the ramp starts from, Q16, rounded up so that the cap holds it
  v.gain_prev = now > v.gain ? now : v.gain;
  v.gain = g;
  v.amp_step = (g * 256 - v.amp) / static_cast<std::int32_t>(kDeclickSamples);
  v.ramp_left = kDeclickSamples;
}

std::uint64_t VoicePool::pitch_inc(unsigned pnote, const SampleDesc& s, int fine, std::int32_t q8) noexcept {
  const std::uint64_t inc = rate_scale_q32(q8 != 0 ? slid_inc_q32(pnote, s.tune, fine, q8) : tuned_inc_q32(pnote, s.tune, fine), s.rate_q28);
  return s.wt_size != 0u ? wavetable_inc_q32(inc, s.wt_size) : inc;
}

void VoicePool::repitch(Voice& v, const SampleDesc& s, int fine) const noexcept {
  std::int32_t q8 = v.slide_q16 >> 4;  // the ramp's offset (1/65536 semitone) in 1/4096 semitone (a floor)
  if (v.slide_left != 0u && ((gliss_ >> (v.track & 15u)) & 1u) != 0u) {
    q8 = ((v.slide_q16 + 32768) >> 16) * 4096;  // OS-COREFX CF2 glissando: a ramp in progress is heard in whole semitones (the nearest)
  }
  // The same slide offset and cent as the last time repitch made `inc`, and nothing else pitch_inc reads has moved since: rp_ok is cleared by the note's start (inc and pnote) and by an edit of the slot's Tune
  // or FineTune (apply_param). set_sample() does NOT clear it: it is the main loop's and rp_ok is the render's; a descriptor is only replaced when no voice sounds on it (voices.h CONTEXTS), and a voice
  // that starts after it clears rp_ok itself.
  if (v.rp_ok && v.rp_q8 == q8 && v.rp_fine == fine) {
    return;
  }
  v.inc = pitch_inc(v.pnote, s, fine, q8);  // OS-RATEPITCH: in flash, with the sample's rate factor
  v.rp_q8 = q8;
  v.rp_fine = static_cast<std::int16_t>(fine);
  v.rp_ok = true;
}

// The voice's filter from the descriptor again (type, cutoff, resonance, on or off): the filter state is kept when the filter stays in the path. The cutoff modulator goes on as it is
// unless it is newly wanted or is another kind now (the descriptor's slot 2 was enabled, disabled, or the filter went on or off), then it starts as at a note.
void VoicePool::refilter(Voice& v) noexcept {
  const SampleDesc& s = samples_[v.slot];
  const unsigned t = v.track;
  const bool was = v.filt;
  const bool was_mod = v.fmod;
  filter_apply(v, s, fx_filter_[t], fx_lfo_[t]);
  if (!was && v.filt) {
    svf_reset(v.svf);
  }
  if (perf_dest(v, 3u, true)) {
    return;  // OS-PERF-LFO: the layer's FLT LFO owns the cutoff modulator (filter_apply left its generator alone)
  }
  ModGen& g = v.mg[kMgCut];
  if (!v.fmod) {
    g.kind = kFmodNone;
  } else if (!was_mod || g.kind != (fx_lfo_[t] != 0u ? kFmodLfo : s.filter.mod.kind)) {
    mod_start(g, s.filter.mod, cursor_step_, 0.0f, fx_lfo_[t]);
  } else if (g.kind == kFmodLfo) {
    lfo_rerate(g, fx_lfo_[t] != 0u ? fx_lfo_[t] - 1u : s.filter.mod.rate);  // (the layer's FLT LFO has let go: the Filter LFO FX's or the instrument's speed)
  }
}

// ---- OS-MOD: the pan, finetune and amp LFO modulators (voices.h PAN, FINETUNE, AMP LFO MODULATION). In flash: per note, and once a block for the voices that have one.

namespace {
// The modulators of a descriptor in the order of the voice's mg[] from kMgPan: pan (slot 1), finetune (slot 5), tremolo (the amp slot, 0), position (slot 3 or 4: OS-GRAN; the descriptor has it only
// for a wavetable or a granular instrument, set_sample). VoicePool::mod_bit is the bit each has in Voice::mmask.
constexpr unsigned kPosMods = 4;
struct ModSlots {
  const FilterMod* m[kPosMods];
};
O3OS_AUDIO_INLINE ModSlots mod_slots(const SampleDesc& s) noexcept { return ModSlots{{&s.pan_mod, &s.tune_mod, &s.amp_mod, &s.pos_mod}}; }
}  // namespace

void VoicePool::mods_start(Voice& v, const SampleDesc& s, bool retrigger, unsigned track, unsigned i) noexcept {
  v.pmask = 0;  // OS-PERF-LFO: a new note starts with the instrument's modulators; perf_dest (end of this function) takes the layer's
  if (v.mode != kModeGranular) {
    v.gstart = static_cast<std::uint32_t>(v.phase >> 32);  // OS-PERFFX P7: the window's start, for tracker_retrigger (gstart is a granular voice's; here in flash: note_on, in SRAM_L, pays no store)
    // OS-PERF-REVERSE (P8, voices.h PERFORMANCE LAYER, PLAYBACK): a one shot or a beat slice note (no loop, not a wavetable) of a track that holds PLAYBACK or the step FX 26 plays its window [gstart, end)
    // backward: run_dir with the window as the loop (loop_len), from the last frame, stopping below the first. Here, in flash, so that note_on (SRAM_L) pays nothing; the render loops know only mode 4.
    if ((((perf_rev_ | step_rev_) >> track) & 1u) != 0u && v.loop_len == 0u && s.wt_size == 0u && v.end > v.gstart) {
      v.mode = kModeReverse;
      v.loop_len = v.end - v.gstart;
      v.phase = static_cast<std::uint64_t>(v.end - 1u) << 32;
      v.bwd = true;
    }
  }
  const ModSlots ms = mod_slots(s);
  pan_dirty_ &= ~(1u << track);  // a pan the voice this note replaces evaluated earlier in the block is not the track's now (mod_block sets it again for a note that has the modulator)
  std::uint8_t mask = 0;
  for (unsigned k = 0; k < kPosMods; ++k) {
    ModGen& g = v.mg[kMgPan + k];
    g.l_ovr = k == 1u ? wave_[0][track] : (k == 2u ? wave_[1][track] : std::uint8_t{0});  // OS-COREFX CF2: the tracker FX's waveform of the vibrato (finetune) and tremolo (amp)
    if (ms.m[k]->kind == kFmodNone) {
      g.kind = kFmodNone;
      continue;
    }
    const float kill = (retrigger && g.kind == kFmodEnv) ? g.e_out : 0.0f;  // an ADSR starts from where the old note's was, as the cutoff's
    mod_start(g, *ms.m[k], cursor_step_, kill, 0u, k == 2u ? kAmpRateBase : 0u);
    mask = static_cast<std::uint8_t>(mask | mod_bit(k));
  }
  v.mmask = mask;
  v.mneed = mask != 0u;
  v.tcur = kTremOne;
  v.tstep = 0;
  const std::int32_t tune = tune_base_q16(track);
  v.slide_q16 = tune;  // OS-PFX: a note starts at its own pitch, and slides at once when its track holds a Slide FX (OS-PERFFX P5: "its own" is the layer's TUNE, the offset's zero)
  v.slide_tgt = tune;
  v.slide_left = 0;
  if (tune != 0) {
    v.mmask = static_cast<std::uint8_t>(v.mmask | kMmSlide);  // the first span makes the pitch of the base (repitch, mod_block), as for a Slide FX
    v.mneed = true;
  }
  if (((slide_on_ >> track) & 1u) != 0u) {
    slide_start(v, slide_fx_[track]);
  }
  glide_start(v, s, track);  // OS-IFX1: from the track's previous note, unless the Slide FX has the ramp
  shape_set(v, s, track);    // OS-IFX1: the voice's overdrive and bit depth
  if (perf_lfo_held(track)) {
    for (unsigned d = 0; d < kLfoDests; ++d) {
      (void)perf_dest(v, d, false);  // OS-PERF-LFO: the layer's LFOs over the instrument's modulators (a fresh phase: the note's)
    }
  }
  // OS-PERF-CPU3: the tail of note_on, here so that note_on (SRAM_L) loses its instructions: last and unconditional (nothing above reads or writes these fields; the early returns of note_on come before the call).
  if (v.env != nullptr) {
    v.releasing = false;
    v.amp_step = 0;
    v.ramp_left = 0;  // the envelope takes the place of the attack ramp
    env_enter(v, kEnvDelay, v.env->delay);  // (HOT, noinline, long_call: a call from flash)
  } else {
    v.env_stage = kEnvOff;
    v.amp_step = (v.gain / static_cast<std::int32_t>(kAttackSamples)) * 256;
    v.ramp_left = kAttackSamples;
  }
  v.last = 0;
  v.dk = 0;  // OS-ODD11: a new note owes no re-entry
  // OS-ODD11: the voice's gate bit is the new track's. A voice reused in the middle of a block (the gated one ended earlier in it, or the oldest voice of a gated track was stolen) would keep its previous occupant's
  // bit, and the new note's first block would not be fed (its attack ramp used up unheard). (gated_ sat in SRAM_L for CPU only, commit 05a5c86e, the flash cost on a tail that was over budget; here it is flash again,
  // not for correctness: keep the order gated_, age, playing.)
  gated_ = static_cast<std::uint16_t>((gated_ & ~(1u << i)) | (((gate_ >> track) & 1u) << i));
  v.age = ++age_;
  v.playing = true;
  v.releasing = false;
  ++stats_.note_ons;
}

// OS-PERFFX P7: a percent of a part of `len` frames, floor, in 32 bits: a positive value counts from the start, a negative one |v| back from the end (f = 100 - |v|). The slot's layer is not held: `fallback`.
std::uint32_t VoicePool::smp_frame(unsigned track, unsigned slot, std::uint32_t len, std::uint32_t fallback) const noexcept {
  if (track >= kVoiceTracks || slot >= kPerfSlots || ((perf_.on[slot] >> track) & 1u) == 0u) {
    return fallback;
  }
  const int v = perf_.val[track][slot];
  const std::uint32_t f = static_cast<std::uint32_t>(v >= 0 ? (v > kPerfPctMax ? kPerfPctMax : v) : (v < -kPerfPctMax ? 0 : kPerfPctMax + v));
  return (len / 100u) * f + ((len % 100u) * f) / 100u;
}

// OS-PERFFX P7: note_on's part of SMP POS and SMP END. A looped sample plays [0, loop_end) before its loop (SMP END ignored), a granular or wavetable instrument has no such part (both ignored). Sets `first` and
// (not looped) `length`, returned as (first << 32) | length so that no reference forces note_on's variables onto its stack. The start at or past the end: the track's note is cut, `ignored` counted and
// kSmpSilent returned, nothing plays.
std::uint64_t VoicePool::smp_window(unsigned track, const SampleDesc& s, bool loops, std::uint32_t length) noexcept {
  std::uint32_t first = 0;
  if (!s.granular && s.wt_size == 0u) {
    const std::uint32_t part = loops ? s.loop_end : length;
    first = smp_frame(track, kPerfPosSlot, part, 0u);
    if (!loops) {
      length = smp_frame(track, kPerfEndSlot, part, length);
    }
    if (first >= (loops ? s.loop_end : length)) {
      cut(track);
      ++stats_.ignored;
      return kSmpSilent;
    }
  }
  return (static_cast<std::uint64_t>(first) << 32) | length;
}

// OS-PERFFX P5: the layer's TUNE of a track as the offset's zero (voices.h PERFORMANCE LAYER, TUNE): semitones x 65536, 0 when the layer holds none.
std::int32_t VoicePool::tune_base_q16(unsigned track) const noexcept {
  if (track >= kVoiceTracks || ((perf_.on[kPerfTuneSlot] >> track) & 1u) == 0u) {
    return 0;
  }
  return static_cast<std::int32_t>(perf_.val[track][kPerfTuneSlot]) * 65536;
}

// OS-PFX, SLIDE FX: the pitch offset goes from where it is to `sixteenths` of a semitone in one step of the tempo (576 ticks, tick_rate_q_ of them a sample in Q24): the one 64-bit
// division of a slide. The ramp is walked by mod_block.
void VoicePool::slide_start(Voice& v, int sixteenths) noexcept {
  v.slide_tgt = slide_target_q16(sixteenths) + tune_base_q16(v.track);  // 1/65536 semitone, around the layer's TUNE (OS-PERFFX P5)
  v.slide_left = slide_step_samples(tick_rate_q_);
  v.mmask = static_cast<std::uint8_t>(v.mmask | kMmSlide);
  v.mneed = true;
}

void VoicePool::fx_event(unsigned track, const SeqEvent& e) noexcept {
  if (track >= kVoiceTracks) {
    return;
  }
  if (e.flags != kModGroupSlide) {
    layer_event(track, e);  // OS-PERFFX P0: the layer's value of Overdrive and Bit depth, then OS-IFX1's Glide, Overdrive, Bit depth; any other group is not the voices'
    return;
  }
  const bool set = e.kind == EventKind::ModSet;
  const unsigned fx_type = set ? e.a : 0u;  // (a ModEnd: no source is left)
  const unsigned value = set ? e.b : 0u;
  const bool on = fx_type == kFxSlideUp || fx_type == kFxSlideDown;
  const int sixteenths = fx_type == kFxSlideDown ? -static_cast<int>(value) : static_cast<int>(value);
  const unsigned i = find(track);
  Voice* const v = (i != kNone && v_[i].playing) ? &v_[i] : nullptr;
  if (!on) {
    slide_on_ = static_cast<std::uint16_t>(slide_on_ & ~(1u << track));
    const std::int32_t tune = tune_base_q16(track);
    if (v != nullptr && (v->slide_q16 != tune || v->slide_left != 0u)) {
      v->slide_q16 = tune;  // the note's own pitch again (OS-PERFFX P5: with the layer's TUNE); mod_block remakes it when the finetune modulator runs, else here
      v->slide_tgt = tune;
      v->slide_left = 0;
      v->mmask = static_cast<std::uint8_t>(v->mmask & ~kMmSlide);
      repitch(*v, samples_[v->slot], samples_[v->slot].fine_tune);
      v->mneed = v->mmask != 0u;
    }
    return;
  }
  const int d = sixteenths < -255 ? -255 : (sixteenths > 255 ? 255 : sixteenths);
  slide_fx_[track] = static_cast<std::int16_t>(d);
  slide_on_ = static_cast<std::uint16_t>(slide_on_ | (1u << track));
  if (v != nullptr) {
    slide_start(*v, d);
  }
}

// ---- OS-COREFX: the tracker FX's two hooks (voices.h, audio/tracker_fx.h). In flash, once a tick for each track that has an effect.

bool VoicePool::tracker_view(unsigned track, TrackerView& out) const noexcept {
  if (track >= kVoiceTracks) {
    return false;
  }
  const unsigned i = find(track);
  if (i == kNone || !v_[i].playing) {
    return false;
  }
  const Voice& v = v_[i];
  out.age = v.age;
  out.note = v.pnote;
  out.rate_hz = rate_hz_of_q28(samples_[v.slot].rate_q28);
  if (v.tvol <= 64u) {
    out.volume = v.tvol;  // the processor gave the voice a volume: it is what the voice plays (v.base then holds it through the note's law, whichever it was)
  } else if (v.bvel) {
    const std::uint32_t vel = (static_cast<std::uint32_t>(v.base) * 127u + 32768u) >> 16;  // the velocity that made base: (vel x 65536 + 63) / 127
    out.volume = static_cast<std::uint8_t>(((vel > 127u ? 127u : vel) + 1u) >> 1);
  } else {
    out.volume = samples_[v.slot].amiga_vol > 64u ? std::uint8_t{64} : samples_[v.slot].amiga_vol;
  }
  return true;
}

void VoicePool::tracker_set(unsigned track, std::int32_t pitch_q16, int volume) noexcept {
  if (track >= kVoiceTracks) {
    return;
  }
  const unsigned i = find(track);
  if (i == kNone || !v_[i].playing) {
    return;
  }
  Voice& v = v_[i];
  if (pitch_q16 != kTrackerKeep) {
    const std::int32_t at = pitch_q16 + tune_base_q16(track);  // OS-PERFFX P5: on top of the layer's TUNE
    v.slide_q16 = at;  // the Slide FX's offset, reached at once: mod_block makes the pitch of it at the next span and lets the flag go (slide_left 0)
    v.slide_tgt = at;
    v.slide_left = 0;
    v.mmask = static_cast<std::uint8_t>(v.mmask | kMmSlide);
    v.mneed = true;
  }
  if (volume >= 0) {
    const unsigned vol = volume > 64 ? 64u : static_cast<unsigned>(volume);
    if (!v.tlaw) {
      const unsigned vel = vol * 2u > 127u ? 127u : vol * 2u;
      v.base = static_cast<std::int32_t>((vel * 65536u + 63u) / 127u);  // note_on's velocity law
    } else {
      v.base = static_cast<std::int32_t>(vol << 10);  // vol / 64 of unity (65536 / 64 = 1024), for as long as the note lasts (CF2); the reference was level 100 through the law (OS-BUG-PASTE24)
    }
    v.tvol = static_cast<std::uint8_t>(vol);
    v.bvel = true;  // regain keeps this base (an instrument edit no longer moves it: the tracker FX owns the voice's level until the note ends)
    regain(v, samples_[v.slot]);
  }
}

// ---- OS-COREFX CF2: retrigger, the vibrato and tremolo waveforms, glissando (voices.h). In flash, once per effect event or per retrigger.

bool VoicePool::tracker_retrigger(unsigned track) noexcept {
  const unsigned i = track < kVoiceTracks ? find(track) : kNone;
  if (i == kNone || !v_[i].playing || v_[i].mode == kModeGranular) {
    return false;
  }
  Voice& v = v_[i];
  carry(v, v.last);  // what the voice was putting out fades under the restart (note_on's retrigger rule)
  if (v.mode == kModeReverse) {
    v.phase = static_cast<std::uint64_t>(v.end - 1u) << 32;  // OS-PERF-REVERSE: a backward note restarts from the window's last frame
    v.bwd = true;
    return true;
  }
  v.phase = static_cast<std::uint64_t>(v.gstart) << 32;  // the window's start (OS-PERFFX P7: the layer's SMP POS at the note's start, else 0)
  v.bwd = false;  // the first pass runs forward (note_on's rule)
  return true;
}

void VoicePool::tracker_wave(unsigned track, unsigned which, unsigned wave) noexcept {
  if (track >= kVoiceTracks || which > 1u) {
    return;
  }
  static constexpr std::uint8_t kShape[4] = {2, 0, 3, 4};  // sine (played as the triangle: this table was made before the Sine shape of OS-LFOSINE, 5, existed; whether wave 0 should use it is open), ramp down (the reverse saw), square, random
  const std::uint8_t ovr = static_cast<std::uint8_t>(kShape[wave & 3u] + 1u);
  wave_[which][track] = ovr;
  const unsigned i = find(track);
  if (i != kNone && v_[i].playing) {
    ModGen& g = v_[i].mg[which == 0u ? kMgTune : kMgTrem];
    g.l_ovr = ovr;
    g.l_q = 0xFFFFFFFFu;  // the random shape draws its level again
  }
}

void VoicePool::tracker_gliss(unsigned track, bool on) noexcept {
  if (track < kVoiceTracks) {
    gliss_ = on ? static_cast<std::uint16_t>(gliss_ | (1u << track)) : static_cast<std::uint16_t>(gliss_ & ~(1u << track));
  }
}

void VoicePool::tracker_reset() noexcept {
  gliss_ = 0;
  step_rev_ = 0;  // OS-PERF-REVERSE: the step FX 26 held ends with the run (the sequencer's table was reset); the layer's PLAYBACK stays
  for (unsigned t = 0; t < kVoiceTracks; ++t) {
    wave_[0][t] = 0;
    wave_[1][t] = 0;
  }
  for (Voice& v : v_) {
    v.mg[kMgTune].l_ovr = 0;
    v.mg[kMgTrem].l_ovr = 0;
  }
}

// ---- OS-IFX1: GLIDE, OVERDRIVE, BIT DEPTH (voices.h). In flash, per note, per live edit and per FX event, except the two passes of shape_span, once a span a voice that uses one.

// A sampler note's glide (voices.h GLIDE): the ramp of the Slide FX from the previous note's pitch (the offset prev - note, in 1/65536 semitone) to 0 over the glide time, the
// instrument's or, while the track holds the Glide FX, the FX's (glide_fx_ms); ms x 44.1 samples, truncated. Nothing for a first note, a repeated one, a time of 0 or a
// note under a held Slide FX (which has the ramp). Also remembers the note for the next one (kept across note ends and stops).
void VoicePool::glide_start(Voice& v, const SampleDesc& s, unsigned track) noexcept {
  const unsigned prev = last_note1_[track];
  last_note1_[track] = static_cast<std::uint8_t>(v.pnote + 1u);
  const std::uint32_t ms = ((ifx_on_[0] >> track) & 1u) != 0u ? glide_fx_ms(ifx_val_[track][0]) : s.glide;
  if (ms == 0u || prev == 0u || prev - 1u == v.pnote || ((slide_on_ >> track) & 1u) != 0u) {
    return;
  }
  const std::int32_t tune = tune_base_q16(track);  // OS-PERFFX P5: the glide runs around the layer's TUNE
  v.slide_q16 = (static_cast<std::int32_t>(prev) - 1 - static_cast<std::int32_t>(v.pnote)) * 65536 + tune;
  v.slide_tgt = tune;
  v.slide_left = glide_samples(ms);
  v.mmask = static_cast<std::uint8_t>(v.mmask | kMmSlide);
  v.mneed = true;
}

// The voice's overdrive gain and bit-depth shift (voices.h OVERDRIVE, BIT DEPTH): the Perform layer's value while it holds one (OS-PERFFX P1), else the track's FX while it holds one, else the
// instrument's; an engine of 12 or 8 bits caps the depth (the larger shift applies) and, when it is the one in force, ROUNDS (crush_round); the Bit Depth parameter / FX truncates. The gain is
// 1 + 15 (value / 100)^2 in Q11 (2048..32768), 0 for none; the shift is 16 - bits (0 for 16 bits). The layer's Bit depth value IS that shift (0..15: the table of seq/perform.h), so its bits are 16 - value.
void VoicePool::shape_set(Voice& v, const SampleDesc& s, unsigned track) const noexcept {
  const unsigned eng = s.engine == kEngineBits12 ? 4u : (s.engine == kEngineBits8 ? 8u : 0u);
  const Shape sh = shape_track(track, s.overdrive, s.bit_depth, eng);
  v.drive = sh.drive;
  v.crush = sh.crush;
  v.crush_round = sh.round;
}

// shape_set's rule for any track (OS-BUG-MODDRIVE: the module instruments' too, Engine::shape_modules): `engine_bits` is the sampler engine's cap, 4 or 8 (0 none).
VoicePool::Shape VoicePool::shape_track(unsigned track, unsigned od_inst, unsigned bits_inst, unsigned engine_bits) const noexcept {
  unsigned od = ((ifx_on_[1] >> track) & 1u) != 0u ? ifx_val_[track][1] : od_inst;
  unsigned bits = ((ifx_on_[2] >> track) & 1u) != 0u ? ifx_val_[track][2] : bits_inst;
  if (perf_held(track, kModGroupOverdrive)) {
    const int pv = perf_value(track, kModGroupOverdrive);
    od = pv < 0 ? 0u : static_cast<unsigned>(pv);
  }
  if (perf_held(track, kModGroupBitDepth)) {
    const int pv = perf_value(track, kModGroupBitDepth);
    bits = 16u - (pv < 0 ? 0u : (pv > 15 ? 15u : static_cast<unsigned>(pv)));
  }
  od = od > 100u ? 100u : od;
  bits = bits < 1u ? 1u : bits;
  bits = bits > 16u ? 16u : bits;
  const unsigned usr = 16u - bits;
  Shape r;
  r.drive = overdrive_gain_q11(od);
  r.crush = static_cast<std::uint8_t>(engine_bits > usr ? engine_bits : usr);
  r.round = static_cast<std::uint8_t>(engine_bits != 0u && engine_bits >= usr);
  return r;
}

namespace {
// The two passes over n samples, the loop-invariant choices as template flags so that each variant is a tight loop. They compute overdrive_sample and bit_depth_sample of voices.h (the
// host tests hold this loop equal to them sample for sample) with the saturation as one SSAT (about 17 cycles a sample for the overdrive, 8 for the depth, 20 for both, [est] from the
// disassembly; `bench voice_od | voice_bit | voice_odbit` measures them).
template <bool Drive, bool Crush, bool Round>
void shape_loop(std::int16_t* io, std::uint32_t n, std::int32_t g, unsigned sh) noexcept {
  const std::int32_t mask = Crush ? ~((std::int32_t{1} << sh) - 1) : 0;
  for (std::uint32_t i = 0; i < n; ++i) {
    std::int32_t x = io[i];
    if constexpr (Drive) {
      const std::int32_t u = simd::NativeOps::ssat16((x * g) >> 12);
      x = (u * (65536 - abs32(u))) >> 15;
    }
    if constexpr (Crush) {
      if constexpr (Round) {
        x = simd::NativeOps::ssat16((x + (std::int32_t{1} << (sh - 1u))) & mask);  // the 12/8-bit engines' rounding, clamped at 32767
      } else {
        x &= mask;  // stays inside 16 bits: only low bits are cleared
      }
    }
    io[i] = static_cast<std::int16_t>(x);
  }
}
}  // namespace

void VoicePool::shape_span(const Voice& v, std::int16_t* io, std::uint32_t n) noexcept { shape_pcm(io, n, v.drive, v.crush, v.crush_round != 0u); }

// The passes over n samples in place (OS-BUG-MODDRIVE: the module instruments' too, one curve). A drive and a depth of none leave the block as it is (the sampler never asks, post_stages; a gain of 0 would zero it).
void VoicePool::shape_pcm(std::int16_t* io, std::uint32_t n, unsigned drive_q11, unsigned crush, bool round) noexcept {
  const std::int32_t g = static_cast<std::int32_t>(drive_q11);
  const unsigned sh = crush;
  if ((drive_q11 | crush) == 0u) {
    return;
  }
  if (sh == 0u) {
    shape_loop<true, false, false>(io, n, g, sh);
  } else if (round) {
    if (g != 0) {
      shape_loop<true, true, true>(io, n, g, sh);
    } else {
      shape_loop<false, true, true>(io, n, g, sh);
    }
  } else if (g != 0) {
    shape_loop<true, true, false>(io, n, g, sh);
  } else {
    shape_loop<false, true, false>(io, n, g, sh);
  }
}

// OS-PERFFX P0: a ModSet / ModEnd of a group other than Slide (voices.h PERFORMANCE LAYER, THE REST OF THE GROUPS). The sequencer's control table sends the WINNER of the group: a ModSet whose layer
// `c` is kLayerPerf has the layer's value in `value` and the step FX underneath in `a` / `b` (a == 0: none); any other ModSet is a step FX and the layer holds nothing; a ModEnd leaves neither. So the
// layer's slot is held exactly when the event is a performance ModSet, and the step layer (ifx_event) is told what is underneath either way. The slot of Overdrive and Bit depth acts since P1:
// ifx_event ends in shape_set, which takes the layer's value over the step FX's and the instrument's, for the voice sounding now and for the track's next notes. The layer's end arrives as the step
// FX again (a ModSet of the columns' layer, a and b being the FX) or as a ModEnd: the slot is let go and the same call brings the step FX's value, or the instrument's, back.
void VoicePool::layer_event(unsigned track, const SeqEvent& e) noexcept {
  const bool set = e.kind == EventKind::ModSet;
  if (e.flags == kModGroupReverse) {
    // OS-PERF-REVERSE (voices.h PERFORMANCE LAYER, PLAYBACK): two sources, each told by every event of the group (a ModEnd lets both go): the layer's PLAYBACK (c == kLayerPerf, value != 0) and the step FX 26
    // (a == kFxReverse, b != 0; under the layer a and b still say the FX underneath). Only the next note reads them (mods_start); a sounding voice is not moved.
    const std::uint16_t bit = static_cast<std::uint16_t>(1u << track);
    const bool layer = set && e.c == kLayerPerf && e.value != 0;
    const bool step = set && e.a == kFxReverse && e.b != 0u;
    perf_rev_ = static_cast<std::uint16_t>(layer ? perf_rev_ | bit : perf_rev_ & ~bit);
    step_rev_ = static_cast<std::uint16_t>(step ? step_rev_ | bit : step_rev_ & ~bit);
    return;
  }
  const unsigned slot = perf_slot_of(e.flags);
  if (slot < kPerfSlots) {
    const bool perf = set && e.c == kLayerPerf;
    const std::int32_t lim = slot == kPerfTuneSlot ? kPerfTuneMax : (slot >= kPerfLfoSlot ? kPerfLfoMax : (slot >= kPerfPosSlot ? kPerfPctMax : 127));
    const std::int32_t v = e.value < -lim ? -lim : (e.value > lim ? lim : e.value);
    const std::int32_t was = slot == kPerfTuneSlot ? tune_base_q16(track) : 0;
    perf_.val[track][slot] = perf ? static_cast<std::int8_t>(v) : std::int8_t{0};
    perf_.on[slot] = static_cast<std::uint16_t>(perf ? perf_.on[slot] | (1u << track) : perf_.on[slot] & ~(1u << track));
    if (slot == kPerfTuneSlot) {
      // OS-PERFFX P5: the voice sounding moves by the difference, offset and target alike (a ramp in progress goes on around the new base); mod_block makes the pitch of it at the next span.
      const std::int32_t d = tune_base_q16(track) - was;
      const unsigned i = find(track);
      if (d != 0 && i != kNone && v_[i].playing) {
        Voice& vo = v_[i];
        vo.slide_q16 += d;
        vo.slide_tgt += d;
        vo.mmask = static_cast<std::uint8_t>(vo.mmask | kMmSlide);
        vo.mneed = true;
      }
    }
  }
  if (e.flags == kModGroupLfoFilter) {
    // The Filter LFO FX 35 (OS-FU2; `a` is the FX when the sequencer holds it, 0 when only the layer's FLT LFO does, `b` its rate): the group used to be handled in Engine::dispatch, where the branches cost SRAM_L bytes.
    // The layer's slot is set above, so the voice sounding takes both here (lfo_set_impl ends in perf_dest).
    lfo_set_impl(*this, track, set && e.a == kFxFilterLfo, e.b);
  } else if (slot >= kPerfLfoSlot && slot < kPerfSlots) {
    // OS-PERF-LFO-STEP: the step FX of the group (23, 24, 33, 36), whoever wins (under the layer a and b still say the FX): its rate index + 1, held to the FX's range; any other ModSet of the group
    // (a == 0: the layer only, or another FX) and a ModEnd let it go. `row` is the destination without the FLT's (3), which is the group 14's own branch above.
    static constexpr std::uint8_t kStepFx[4] = {kFxLfoVolume, kFxLfoPanning, kFxLfoFinetune, kFxLfoPosition};
    const unsigned row = slot - kPerfLfoSlot == 4u ? 3u : slot - kPerfLfoSlot;
    const unsigned top = row == 0u ? kAmpLfoRates - 1u : kLfoRates - 1u;
    step_lfo_[row][track] = (set && e.a == kStepFx[row]) ? static_cast<std::uint8_t>((e.b > top ? top : e.b) + 1u) : std::uint8_t{0};
    const unsigned i = find(track);
    if (i != kNone && v_[i].playing) {
      perf_sync(v_[i], slot - kPerfLfoSlot);  // OS-PERF-LFO: the LFO of the destination starts, moves or ends on the sounding voice
    }
  }
  ifx_event(track, e.flags, set ? e.a : 0u, set ? e.b : 0u);
}

// OS-PERFFX P0, the export (voices.h): the layer put away, cleared, and brought back. The performance filter is FilterFx's own: perf_filter_to hands the track's whole filter state to set_filter with the
// layer's part changed (the sounding voice follows), for the tracks where it differs.
void VoicePool::perf_filter_to(unsigned track, unsigned type_plus_one, int offset) noexcept {
  const FilterFx& fx = fx_filter_[track];
  if (fx.perf_type != type_plus_one || fx.perf_off != offset) {
    set_filter(track, fx.on, fx.type, static_cast<unsigned>(fx.cutoff * 100.0f + 0.5f), type_plus_one, offset);
  }
}

void VoicePool::perf_save(PerfSave& out) const noexcept {
  out.slots = perf_;
  out.rev = perf_rev_;
  std::memcpy(out.step_lfo, step_lfo_, sizeof(step_lfo_));
  for (unsigned t = 0; t < kVoiceTracks; ++t) {
    out.filter_type[t] = fx_filter_[t].perf_type;
    out.filter_off[t] = fx_filter_[t].perf_off;
  }
}

void VoicePool::perf_clear() noexcept {
  perf_ = PerfSlots{};
  perf_rev_ = 0;
  step_rev_ = 0;  // (the export's own step FX 26 are told afresh by its sequencer)
  std::memset(step_lfo_, 0, sizeof(step_lfo_));  // OS-PERF-LFO-STEP: the export has no step LFO but its own (its FxReset ends them too, but not relied on)
  for (unsigned t = 0; t < kVoiceTracks; ++t) {
    perf_filter_to(t, 0u, 0);
  }
}

void VoicePool::perf_restore(const PerfSave& in) noexcept {
  perf_ = in.slots;
  perf_rev_ = in.rev;
  step_rev_ = 0;  // the export's step FX 26 end with it; the live sequencer tells its own again at its next transport start
  std::memcpy(step_lfo_, in.step_lfo, sizeof(step_lfo_));  // OS-PERF-LFO-STEP: the live step LFOs are back (no voice sounds: the export's start ended them)
  for (unsigned t = 0; t < kVoiceTracks; ++t) {
    perf_filter_to(t, in.filter_type[t], in.filter_off[t]);
  }
}

// A ModSet / ModEnd of the Glide, Overdrive or Bit depth group: the track holds the FX (its value kept) or lets it go. The Glide is read at the next note; the other two reach the voice
// sounding now (the instrument's value comes back with the ModEnd). Any other group is not the voices'.
void VoicePool::ifx_event(unsigned track, unsigned group, unsigned fx_type, unsigned value) noexcept {
  unsigned k;
  unsigned type;
  switch (group) {
    case kModGroupGlide: k = 0; type = kFxGlide; break;
    case kModGroupOverdrive: k = 1; type = kFxOverdrive; break;
    case kModGroupBitDepth: k = 2; type = kFxBitDepth; break;
    default: return;
  }
  const std::uint16_t bit = static_cast<std::uint16_t>(1u << track);
  if (fx_type == type) {
    const unsigned lo = k == 2u ? 1u : 0u;
    const unsigned hi = k == 2u ? 16u : 100u;
    ifx_val_[track][k] = static_cast<std::uint8_t>(value < lo ? lo : (value > hi ? hi : value));
    ifx_on_[k] = static_cast<std::uint16_t>(ifx_on_[k] | bit);
  } else {
    ifx_on_[k] = static_cast<std::uint16_t>(ifx_on_[k] & ~bit);
  }
  if (k != 0u) {
    const unsigned i = find(track);
    if (i != kNone && v_[i].playing) {
      shape_set(v_[i], samples_[v_[i].slot], track);
    }
  }
}

// The position the modulator's output `mod` (-1..1) says: the instrument's window plus mod x the windows, or the grain's position plus mod x 65535, each cut to a whole number toward zero and held to
// the stack / 0..65535 (whether the official firmware computes it the same way is Q). A wavetable voice reads the window from now on; a granular voice takes the position at its next wrap (a pending one).
void VoicePool::move_position(Voice& v, const SampleDesc& s, float mod) noexcept {
  if (v.mode == kModeGranular) {
    int pos = static_cast<int>(s.gr_pos) + static_cast<int>(mod * 65535.0f);
    pos = pos < 0 ? 0 : (pos > 65535 ? 65535 : pos);
    v.gnew = static_cast<std::uint16_t>(pos);
    v.gpend = v.gnew != v.gpos;
  } else if (s.wt_windows > 1u) {
    const std::int64_t last = static_cast<std::int64_t>(s.wt_windows) - 1;
    std::int64_t win = static_cast<std::int64_t>(s.wt_pos) + static_cast<std::int64_t>(mod * static_cast<float>(s.wt_windows));
    win = win < 0 ? 0 : (win > last ? last : win);
    v.data = s.data + (win - static_cast<std::int64_t>(s.wt_pos)) * static_cast<std::int64_t>(s.wt_size);
  }
}

// OS-ODD11 (voices.h GATE): a gated voice's cutoff ADSR keeps its place. `since` is 16 bits and nothing else folds it while the filter is skipped, so it is folded into the envelope (fenv_eval) once
// it has reached a block; unmuted after any time, the envelope stands where an ungated voice's would. The LFO keeps no clock here (it reads the transport's).
void VoicePool::gate_cut(Voice& v, std::uint32_t n) noexcept {
  if (!v.fmod) {
    return;
  }
  ModGen& g = v.mg[kMgCut];
  if (g.kind != kFmodEnv) {
    return;
  }
  g.since = static_cast<std::uint16_t>(g.since + n);  // below kBlock + kBlock before the add: no wrap
  if (g.since >= kBlock) {
    (void)fenv_eval(g, samples_[v.slot].filter.mod);
  }
  g.need = true;  // the first span after the gate evaluates afresh
}

// OS-ODD11 (voices.h GATE): a span of n samples in which the voice skipped its post stages: it is not fed, it owes the re-entry, and the cutoff envelope's clock goes on.
O3OS_AUDIO_INLINE void VoicePool::gate_mark(Voice& v, unsigned i, std::uint32_t n) noexcept {
  gated_ = static_cast<std::uint16_t>(gated_ | (1u << i));
  v.dk = kOwed;
  gate_cut(v, n);
}

// OS-ODD11 slice 2 (voices.h GATE, CLOSED-FORM ADVANCE): a span of a gated voice that is of the plain kind (a one shot or a forward loop, no residual tail, not a wavetable), taken in closed form: the
// block is not zeroed or written, mod_block runs as it would (the pan, the finetune and the slide move the voice's increment, once a block), the envelope segments are render_voice's with advance() for the
// sample loop, and the gate's bookkeeping is odd_span's. True: done. False: another kind, nothing touched (render goes on as in slice 1). Flash: one call a gated voice and span.
bool VoicePool::gated_span(Voice& v, std::uint32_t begin, std::uint32_t end) noexcept {
  const SampleDesc& s = samples_[v.slot];
  if (v.mode != 0u || v.resid != 0 || s.wt_size != 0u) {
    return false;
  }
  const unsigned i = static_cast<unsigned>(&v - v_);  // (the caller's index is not passed: a fourth argument keeps the call in registers)
  const bool first = (touched_ & (kSeen << i)) == 0u;
  touched_ |= 1u << (kVisit + i);
  if (v.mmask != 0u && (first || v.mneed)) {
    mod_block(v, s, begin);
  }
  v.last = 0;  // the voice put nothing out: a stop or a retrigger carries no tail
  render_seg<true>(v, nullptr, begin, end);
  gate_mark(v, i, end - begin);
  __atomic_store_n(&stats_.advanced_spans, stats_.advanced_spans + 1u, __ATOMIC_RELAXED);  // (`diag voices` reads it from the main loop: voice_snapshot())
  return true;
}

// OS-ODD11 (voices.h GATE): a span of a voice that is gated (`gate`), or that owes its re-entry (dk != 0: the gate lifted, a few spans in all). Flash (O3OS_AUDIO_COLD), so that the unmuted render pays two tests a
// voice and nothing more; a muted scene calls it once per gated voice and span (as it called gate_cut), the span of a lift runs the stages from here.
void VoicePool::odd_span(Voice& v, unsigned i, std::int16_t* block, std::uint32_t begin, std::uint32_t end, bool gate) noexcept {
  if (gate) {
    gate_mark(v, i, end - begin);
    return;
  }
  if (v.dk == kOwed) {  // the gate lifted: the post stages start cold, under a short ramp (the strip opened with no feed of this voice)
    v.dk = static_cast<std::uint8_t>(v.playing ? kAttackSamples : 0u);
    svf_reset(v.svf);
  }
  post_stages(v, block, begin, end);
  if (v.dk != 0u) {  // the re-entry ramp, linear from silence over kAttackSamples (the rest of the ramp in the span that follows)
    const std::uint32_t span = end - begin;
    const std::uint32_t m = v.dk < span ? v.dk : span;
    std::int16_t* const o = block + begin;
    // The kernel's ramp is (i + 1) / kBlock of the way from start to end: start = p / kAttackSamples, end = start + 2 (kBlock = 2 x kAttackSamples) makes the gain of sample i (p + i + 1) / kAttackSamples.
    static_assert(kBlock == 2u * kAttackSamples, "the re-entry ramp is the kernel's ramp over half a block");
    const std::int32_t p = static_cast<std::int32_t>(kAttackSamples - v.dk);
    const std::int32_t start = p * (kernel::kUnity / static_cast<std::int32_t>(kAttackSamples));
    kernel::gain(o, o, m, start, start + 2 * kernel::kUnity);
    v.dk = static_cast<std::uint8_t>(v.dk - m);
  }
}

// The pan and the finetune of a voice that has them, from the modulators' output now (the span begins at `begin` of the block). Our model (pan and finetune): the output (-1..1) times 100 is truncated to a whole number, added to the instrument's value and held to 0..100 / -100..100.
void VoicePool::mod_block(Voice& v, const SampleDesc& s, std::uint32_t begin) noexcept {
  v.mneed = false;
  const std::uint16_t ahead = static_cast<std::uint16_t>(kBlock - begin);  // samples to the next block's start, where an ADSR is evaluated next
  if ((v.mmask & kMmSlide) != 0u && (v.mmask & kMmTune) == 0u) {
    repitch(v, s, s.fine_tune);  // OS-PFX: this block's pitch, the offset where the ramp stands (with the finetune modulator the pitch is made below, with the same offset)
  }
  if ((v.mmask & kMmPan) != 0u) {
    ModGen& g = v.mg[kMgPan];
    const float mod = g.kind == kFmodEnv ? fenv_eval(g, s.pan_mod) : flfo_eval(g, dmod(v, 1u, s.pan_mod), 0u);
    g.since = ahead;
    const int pan = static_cast<int>(s.pan) + static_cast<int>(mod * 100.0f);
    pan_val_[v.track < kVoiceTracks ? v.track : 0u] = static_cast<std::uint8_t>(pan < 0 ? 0 : (pan > 100 ? 100 : pan));
    pan_dirty_ |= 1u << (v.track < kVoiceTracks ? v.track : 0u);
  }
  if ((v.mmask & kMmTune) != 0u) {
    ModGen& g = v.mg[kMgTune];
    const float mod = g.kind == kFmodEnv ? fenv_eval(g, s.tune_mod) : flfo_eval(g, dmod(v, 2u, s.tune_mod), 0u);
    g.since = ahead;
    const int fine = static_cast<int>(s.fine_tune) + static_cast<int>(mod * 100.0f);
    repitch(v, s, fine < -100 ? -100 : (fine > 100 ? 100 : fine));
  }
  if ((v.mmask & kMmPos) != 0u) {  // OS-GRAN: the wavetable's window or the granular position (voices.h POSITION MODULATION)
    ModGen& g = v.mg[kMgPos];
    const float mod = g.kind == kFmodEnv ? fenv_eval(g, s.pos_mod) : flfo_eval(g, dmod(v, 4u, s.pos_mod), 0u);
    g.since = ahead;
    move_position(v, s, mod);
  }
  if ((v.mmask & kMmTrem) != 0u) {  // the tremolo's multiplier at the block's grid points (a point before the span's start is not used), and at the span's first sample
    ModGen& g = v.mg[kMgTrem];
    const FilterMod& m = dmod(v, 0u, s.amp_mod);
    const bool on = g.kind == kFmodLfo;  // (OS-PERF-LFO: the generator's kind, not the descriptor's: the layer's tremolo runs over an amp slot that is Off or an envelope; remod and mods_start keep the two equal otherwise)
    const auto level = [&](std::uint32_t after) {
      const float l = on ? flfo_level(g, m, after) : 1.0f;  // -1..1 (a negative amount): held to 0..1, the gain of a voice has no sign
      return l > 0.0f ? static_cast<std::int32_t>(l * static_cast<float>(kTremOne)) : 0;
    };
    for (std::uint32_t k = 0; k < kBlock / kFilterCtl; ++k) {
      v.tgt[k] = k * kFilterCtl >= begin ? level(k * kFilterCtl - begin) : kTremOne;
    }
    v.t0 = (begin & (kFilterCtl - 1u)) != 0u ? level(0u) : kTremOne;
    v.mmask = on ? static_cast<std::uint8_t>(v.mmask & ~kMmTremOff) : static_cast<std::uint8_t>(v.mmask | kMmTremOff);
  }
  if ((v.mmask & kMmSlide) != 0u) {  // OS-PFX: the ramp moves on to the next evaluation, `ahead` samples (linear: what is left of the distance over what is left of the time); it lands with the last one
    if (v.slide_left == 0u) {
      v.mmask = static_cast<std::uint8_t>(v.mmask & ~kMmSlide);  // the pitch above had the target
    } else {
      PitchRamp r{v.slide_q16, v.slide_tgt, v.slide_left};  // the law of audio/pitch_ramp.h, the module instruments' too: it lands with the last evaluation, the next block's puts the target into the pitch
      ramp_advance(r, ahead);
      v.slide_q16 = r.q16;
      v.slide_left = r.left;
    }
  }
}

// A live edit made a modulator of the slot on or off, or another kind (ADSR / LFO): the voices that play the slot start it as at a note, or end it (pan and finetune back at the instrument's value).
void VoicePool::remod(Voice& v, std::uint32_t& pan_tracks) noexcept {
  for (unsigned k = 0; k < kPosMods; ++k) {
    remod_k(v, k, pan_tracks);
  }
}

// ... one of the four (k: pan, finetune, amp, position). OS-PERF-LFO: a destination the layer's LFO owns (perf_dest) is left as it is, whatever the edit; one it has just let go (perf_sync) comes here and goes back
// to the instrument's modulator, its LFO at the instrument's own speed.
void VoicePool::remod_k(Voice& v, unsigned k, std::uint32_t& pan_tracks) noexcept {
  static constexpr std::uint8_t kDestOfK[kPosMods] = {1, 2, 0, 4};  // the Perform LFO destination (0 VOL, 1 PAN, 2 TUNE, 3 FLT, 4 POS) of mod_slots' k
  if (perf_dest(v, kDestOfK[k], true)) {
    return;
  }
  const SampleDesc& s = samples_[v.slot];
  const ModSlots ms = mod_slots(s);
  ModGen& g = v.mg[kMgPan + k];
  const FilterMod& m = *ms.m[k];
  const std::uint8_t bit = mod_bit(k);
  const bool running = (v.mmask & bit) != 0u && g.kind != kFmodNone;
  if (m.kind == kFmodNone) {
    if (!running) {
      return;
    }
    g.kind = kFmodNone;
    if (k == 0u) {
      v.mmask = static_cast<std::uint8_t>(v.mmask & ~bit);
      pan_tracks |= 1u << v.track;  // the caller gives the track the instrument's pan again
    } else if (k == 1u) {
      v.mmask = static_cast<std::uint8_t>(v.mmask & ~bit);
      repitch(v, s, s.fine_tune);
    } else if (k == 3u) {
      v.mmask = static_cast<std::uint8_t>(v.mmask & ~bit);
      move_position(v, s, 0.0f);  // back at the instrument's window or position
    }  // the tremolo's pass ramps back to unity by itself and then ends
  } else if (!running || g.kind != m.kind) {
    mod_start(g, m, cursor_step_, 0.0f, 0u, k == 2u ? kAmpRateBase : 0u);
    v.mmask = static_cast<std::uint8_t>(v.mmask | bit);
    if (k == 2u) {
      g.need = false;  // the multiplier ramps from where it is to the LFO's level at the next grid point (mod_block's, at the next block)
    } else {
      v.mneed = true;
    }
  } else if (m.kind == kFmodLfo) {
    lfo_rerate(g, m.rate + (k == 2u ? kAmpRateBase : 0u));  // the layer's LFO has let go: the instrument's own speed (nothing to do when it never moved)
  }
}

// ---- OS-PERF-LFO: the Perform LFO rows (voices.h PERFORMANCE LAYER, THE PERFORM LFOS; docs/specs/OS-PERFFX.md section 11). In flash, at events only (a note start, a layer or instrument edit), never per block.

namespace {
// The five destinations in the order of the Perform rows (VOL, PAN, TUNE, FLT, POS LFO): the voice's generator, mod_slots' index of its modulator (4: the cutoff's, FilterDesc::mod) and the table index of the slot's rate 0
// (the amp slot's table is the last 24 of the 29).
struct LfoDest {
  std::uint8_t gen;
  std::uint8_t k;
  std::uint8_t rate0;
};
constexpr LfoDest kLfoDest[VoicePool::kLfoDests] = {{kMgTrem, 2, kAmpRateBase}, {kMgPan, 0, 0}, {kMgTune, 1, 0}, {kMgCut, 4, 0}, {kMgPos, 3, 0}};
O3OS_AUDIO_INLINE const FilterMod& dest_mod(const SampleDesc& s, unsigned d) noexcept { return kLfoDest[d].k == 4u ? s.filter.mod : *mod_slots(s).m[kLfoDest[d].k]; }
}  // namespace

// The table index (0..28) of the layer's LFO for destination `d` on the voice: the index of the instrument's LFO in that slot, or "1 step" when the slot has none (Off or an envelope), plus the layer's offset, held to the
// table (the amp's: 5..28). -1: the layer holds none on the track, or it cannot act: FLT needs a filter that runs (the instrument's, a Cutoff FX's or a held one), POS a wavetable or granular instrument.
int VoicePool::perf_index(const Voice& v, unsigned d) const noexcept {
  const unsigned t = v.track;
  const unsigned slot = kPerfLfoSlot + d;
  if (d >= kLfoDests || t >= kVoiceTracks) {
    return -1;
  }
  // The layer's offset, 0 when it holds none or holds 0 (OS-PERF-LFO-STEP: then the step FX 23 / 24 / 33 / 36's absolute rate index + 1, `step`, if the track holds it; FLT's is fx_lfo_, not here)
  const int off = ((perf_.on[slot] >> t) & 1u) != 0u ? perf_.val[t][slot] : 0;
  const unsigned step = d == 3u ? 0u : step_lfo_[d == 4u ? 3u : d][t];
  if (off == 0 && step == 0u) {
    return -1;
  }
  const SampleDesc& s = samples_[v.slot];
  if ((d == 3u && !v.filt) || (d == 4u && !(s.granular || (s.wt_size != 0u && s.wt_windows > 1u)))) {
    return -1;
  }
  const FilterMod& m = dest_mod(s, d);
  const int rate0 = kLfoDest[d].rate0;
  const int i = off == 0 ? static_cast<int>(step) - 1 + rate0 : (m.kind == kFmodLfo ? static_cast<int>(m.rate) + rate0 : static_cast<int>(kPerfLfoBase)) + off;
  return i < rate0 ? rate0 : (i > static_cast<int>(kLfoRates) - 1 ? static_cast<int>(kLfoRates) - 1 : i);
}

// Destination `d` of the voice follows the layer: true when the layer owns it (the generator is an LFO at perf_index, started as at a note, or moved to the new speed with its phase kept), false when the layer holds
// nothing there (nothing is touched). The instrument's own shape and amount (FilterMod::shape, lfo_amount: kept in the descriptor whatever the slot's source is) are the LFO's. `live` false: a note's start.
bool VoicePool::perf_dest(Voice& v, unsigned d, bool live) noexcept {
  const int idx = perf_index(v, d);
  if (idx < 0) {
    v.pmask = static_cast<std::uint8_t>(v.pmask & ~(1u << d));
    return false;
  }
  const LfoDest& ds = kLfoDest[d];
  ModGen& g = v.mg[ds.gen];
  {
    // A slot that is Off or an ADSR plays its stored shape and amount; one that stores nothing audible (an old project, an OG file: 0 or a negative amount, a NaN) plays kPerfLfoMod (a triangle at 50 %).
    const FilterMod& sm = dest_mod(samples_[v.slot], d);
    const bool fallback = sm.kind != kFmodLfo && !(sm.lfo_amount >= kPerfLfoMinAmount);
    v.pmask = fallback ? static_cast<std::uint8_t>(v.pmask | (1u << d)) : static_cast<std::uint8_t>(v.pmask & ~(1u << d));
  }
  const std::uint8_t bit = d == 3u ? std::uint8_t{0} : mod_bit(ds.k);
  if (live && g.kind == kFmodLfo && (d == 3u || (v.mmask & bit) != 0u)) {
    lfo_rerate(g, static_cast<unsigned>(idx));
  } else {
    mod_start(g, dest_mod(samples_[v.slot], d), cursor_step_, 0.0f, static_cast<unsigned>(idx) + 1u);
    v.mmask = static_cast<std::uint8_t>(v.mmask | bit);
    if (d == 0u) {
      g.need = !live;  // the multiplier ramps from where it is to the LFO's level at the next grid point (a note's first sample has no ramp)
    } else if (d != 3u) {
      v.mneed = true;
    }
  }
  if (d == 3u) {
    v.fmod = true;  // (the hot filter pass evaluates the cutoff's modulator when this is set: filter_run, post_stages)
  }
  return true;
}

// The layer's value of destination `d` changed on the track (layer_event): the sounding voice starts, moves or ends the LFO. Ended (or unable to act): the instrument's own modulator comes back, as a live SOURCE edit
// makes it (refilter for the cutoff, remod_k for the others; the pan goes to the strip at the next block through pan_val_).
void VoicePool::perf_sync(Voice& v, unsigned d) noexcept {
  if (perf_dest(v, d, true)) {
    return;
  }
  if (d == 3u) {
    refilter(v);
    return;
  }
  static constexpr std::uint8_t kKOfDest[kLfoDests] = {2, 0, 1, 4, 3};  // mod_slots' index of the destination (4: unused)
  std::uint32_t pan_tracks = 0;
  remod_k(v, kKOfDest[d], pan_tracks);
  if (pan_tracks != 0u && v.track < kVoiceTracks) {
    pan_val_[v.track] = static_cast<std::uint8_t>(samples_[v.slot].pan);  // the instrument's pan comes back
    pan_dirty_ |= 1u << v.track;
  }
}

// OS-MOD, LIVE LOOP (voices.h): a live edit of the loop's start or end, `value` in samples from the descriptor's data. Render context. The descriptor's loop moves, held to [0, length] and
// 2 samples long; the voices that loop over it take the new loop at once, which is one block from the edit. Valid (true) whatever it did: a descriptor with no loop, a wavetable or a beat
// slice has none to move, and the sample bank's handover (a rebuild from the project) says what it is when playback stops.
bool VoicePool::apply_loop(unsigned slot, unsigned field, float value) noexcept {
  SampleDesc& d = samples_[slot];
  if (d.length == 0u || d.wt_size != 0u || d.beat_slice || d.loop_end - d.loop_start < 2u) {
    return true;
  }
  const std::uint32_t at = static_cast<std::uint32_t>(whole(value, 0, static_cast<int>(d.length > 0xFFFFFFu ? 0xFFFFFFu : d.length)));
  std::uint32_t ls = d.loop_start;
  std::uint32_t le = d.loop_end;
  if (field == inst_param::LoopStart) {
    ls = at > le - 2u ? le - 2u : at;
  } else {
    le = at < ls + 2u ? ls + 2u : (at > d.length ? d.length : at);
  }
  d.loop_start = ls;
  d.loop_end = le;
  for (unsigned i = 0; i < kMaxVoices; ++i) {
    Voice& v = v_[i];
    if (!(v.playing || v.resid != 0) || v.slot != slot || v.loop_len == 0u) {
      continue;
    }
    v.end = le;
    v.loop_len = le - ls;
    if (v.mode != 0u) {  // backward / ping-pong: a position past the new end is put at the end (the turn), so that a backward voice does not hold the last sample until it comes down to it
      const std::uint64_t top = static_cast<std::uint64_t>(le) << 32;
      v.phase = v.phase >= top ? top - 1u : v.phase;
    }  // a forward voice wraps by itself: the sample loop takes a position at or past the end back into the loop
  }
  return true;
}

// OS-GRAN: a live edit of the wavetable window or the granular position, `value` a whole number. Render context; the descriptor's `data` moves to the window (the stack's start does not),
// and the voices that play it take it at once: a wavetable voice reads the new window from its next sample, a granular voice
// takes the position at its grain's next wrap (voices.h GRANULAR). A slot that has no such window or grain ignores it (valid: the sample bank's handover says what the slot is).
bool VoicePool::apply_position(unsigned slot, unsigned field, float value) noexcept {
  SampleDesc& d = samples_[slot];
  if (d.length == 0u) {
    return true;
  }
  if (field == inst_param::WtWindow) {
    if (d.wt_size == 0u || d.wt_windows == 0u) {
      return true;
    }
    const std::uint32_t w = static_cast<std::uint32_t>(whole(value, 0, static_cast<int>(d.wt_windows > 0x7FFFFFFFu ? 0x7FFFFFFFu : d.wt_windows) - 1));
    const std::int32_t moved = static_cast<std::int32_t>(w) - static_cast<std::int32_t>(d.wt_pos);
    d.data += static_cast<std::ptrdiff_t>(moved) * d.wt_size;
    d.offset += static_cast<std::uint32_t>(moved * static_cast<std::int32_t>(d.wt_size));
    d.wt_pos = w;
    for (unsigned i = 0; i < kMaxVoices; ++i) {
      Voice& v = v_[i];
      if ((v.playing || v.resid != 0) && v.slot == slot && v.mode != kModeGranular) {
        v.data = d.data;  // (a window of its own under the position modulator is re-made at its next block)
      }
    }
  } else {
    if (!d.granular) {
      return true;
    }
    d.gr_pos = static_cast<std::uint16_t>(whole(value, 0, 65535));
    for (unsigned i = 0; i < kMaxVoices; ++i) {
      Voice& v = v_[i];
      if (v.playing && v.slot == slot && v.mode == kModeGranular) {
        v.gnew = d.gr_pos;
        v.gpend = v.gnew != v.gpos;
      }
    }
  }
  return true;
}

// OS-LFOLIVE: what a live edit of a slot's SOURCE or LFO field does to the voices that play the slot (the descriptor already has it: apply_lfo_field). Render context, once per ParamChange, never
// per block. SHAPE and AMOUNT need nothing: every evaluation reads them from the descriptor (flfo_level), so the voice has them at its next one (the cutoff's every 16th sample, the others'
// at the next block). SPEED is the one value a voice keeps a derivative of, the period of its LFO in ticks and its reciprocal: made again here, without the note's other work (the phase is
// the transport's, so the LFO is where the new period puts it). SOURCE starts or ends the modulator as an ENABLE edit does (refilter, remod).
void VoicePool::lfo_live(unsigned slot, unsigned e, unsigned f, std::uint32_t& pan_tracks) noexcept {
  static constexpr std::uint8_t kDestOfE[inst_param::kEnvelopes] = {0, 1, 3, 4, 4, 2};  // the Perform LFO destination of the instrument's slot e
  if (f == inst_param::LfoShape || f == inst_param::LfoAmount) {
    if (f == inst_param::LfoAmount) {  // OS-PERF-LFO: the amount decides whether the layer's LFO plays the stored slot or kPerfLfoMod (Voice::pmask): taken again, or the sounding note keeps the old choice
      for (unsigned i = 0; i < kMaxVoices; ++i) {
        Voice& v = v_[i];
        if (v.playing && v.slot == slot && perf_index(v, kDestOfE[e]) >= 0) {
          (void)perf_dest(v, kDestOfE[e], true);
        }
      }
    }
    return;
  }
  const SampleDesc& s = samples_[slot];
  const FilterMod* const mods[inst_param::kEnvelopes] = {&s.amp_mod, &s.pan_mod, &s.filter.mod, &s.pos_mod, &s.pos_mod, &s.tune_mod};
  static constexpr std::uint8_t kGen[inst_param::kEnvelopes] = {kMgTrem, kMgPan, kMgCut, kMgPos, kMgPos, kMgTune};
  for (unsigned i = 0; i < kMaxVoices; ++i) {
    Voice& v = v_[i];
    if (!v.playing || v.slot != slot) {
      continue;
    }
    if (f == inst_param::LfoSource) {
      if (e == 2u) {
        refilter(v);
      } else {
        remod(v, pan_tracks);
      }
      continue;
    }
    ModGen& g = v.mg[kGen[e]];
    const int owned = perf_index(v, kDestOfE[e]);  // OS-PERF-LFO: the layer's speed index over the instrument's (its offset is on the instrument's new speed)
    if (g.kind != kFmodLfo || (e == 2u && owned < 0 && fx_lfo_[v.track] != 0u)) {  // (the Filter LFO FX has the cutoff's rate while the track holds it)
      continue;
    }
    lfo_rerate(g, owned >= 0 ? static_cast<unsigned>(owned) : mods[e]->rate + (e == 0u ? kAmpRateBase : 0u));
  }
}

// An LFO that runs moves to table index `idx` (0..28): its period and the reciprocal are made again, and the phase u = (x mod old period) / old period is kept: the shift is made again for the new period (once per
// edit and voice), or the phase would jump at every step of the encoder. Nothing when the period is the same. Flash, never per block.
void VoicePool::lfo_rerate(ModGen& g, unsigned idx) noexcept {
  const std::uint32_t period = static_cast<std::uint32_t>(static_cast<float>(kTicksPerStep) * kLfoRate[idx < kLfoRates ? idx : kLfoRates - 1u]);
  const std::uint32_t np = period != 0u ? period : 1u;
  const std::uint32_t op = g.l_period;
  if (np == op) {
    return;
  }
  const std::uint32_t base = tick_base();
  const std::uint32_t r = static_cast<std::uint32_t>(static_cast<std::uint64_t>((base + g.l_shift) % op) * np / op);  // the phase, in new ticks
  g.l_period = np;
  g.l_inv = 1.0f / static_cast<float>(np);
  g.l_shift = (r + np - base % np) % np;
  g.l_q = g.l_q == 0xFFFFFFFFu ? g.l_q : (base + g.l_shift) / np;  // (a random level already drawn is held)
}

bool VoicePool::apply_param(unsigned slot, unsigned field, float value, std::uint32_t& pan_tracks) noexcept {
  pan_tracks = 0;
  if (slot >= kSampleSlots || !(value == value)) {
    return false;  // (a NaN is no value; the descriptor must not take it)
  }
  if (field == inst_param::LoopStart || field == inst_param::LoopEnd) {
    return apply_loop(slot, field, value);
  }
  if (field == inst_param::WtWindow || field == inst_param::GranPos) {
    return apply_position(slot, field, value);
  }
  SampleDesc empty;  // an empty slot has no descriptor to keep: the field is only checked, and set_sample() builds the slot from the project when audio is bound
  SampleDesc& d = samples_[slot].length != 0u ? samples_[slot] : empty;
  const InstApply r = apply_instrument_param(d, field, value, law_);
  if (r != InstApply::Applied || samples_[slot].length == 0u) {
    return r != InstApply::Unknown;
  }
  using namespace inst_param;
  if (is_lfo_field(field)) {
    lfo_live(slot, (field - kLfoFirst) / kLfoStride, (field - kLfoFirst) % kLfoStride, pan_tracks);
    return true;
  }
  const bool gain = field == Volume || field == envelope(0, EnvAmount);
  const bool pitch = field == Tune || field == FineTune;
  const bool filter = (field >= FilterEnable && field <= Resonance) || field == envelope(2, EnvEnable);
  const bool shape = field == Overdrive || field == BitDepth;  // OS-IFX1: the glide is read at a note
  const bool mods = field == envelope(0, EnvEnable) || field == envelope(1, EnvEnable) || field == envelope(3, EnvEnable) || field == envelope(4, EnvEnable) || field == envelope(5, EnvEnable);
  for (unsigned i = 0; i < kMaxVoices; ++i) {
    Voice& v = v_[i];
    if (!v.playing || v.slot != slot) {
      continue;
    }
    if (gain) {
      regain(v, d);
    } else if (pitch) {
      v.rp_ok = false;  // (the descriptor's Tune or FineTune moved: pitch_inc's other inputs changed under the key)
      // the finetune modulator's offset is the voice's next block's to add (mod_block), so the base alone is made here
      repitch(v, d, d.fine_tune);
    } else if (filter) {
      refilter(v);
    } else if (shape) {
      shape_set(v, d, v.track);
    } else if (mods) {
      remod(v, pan_tracks);
    } else if (field == Panning && (v.mmask & kMmPan) == 0u) {
      pan_tracks |= 1u << v.track;  // a voice with a pan modulator takes the instrument's pan at its next block (mod_block)
    }
  }
  return true;
}

std::uint32_t VoicePool::slot_tracks(unsigned slot) const noexcept {
  std::uint32_t mask = 0;
  for (unsigned i = 0; i < kMaxVoices; ++i) {
    if (v_[i].playing && v_[i].slot == slot) {
      mask |= 1u << v_[i].track;
    }
  }
  return mask;
}

}  // namespace o3os::audio
