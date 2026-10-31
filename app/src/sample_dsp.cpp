// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/sample_dsp.h>

#include <o3os/app/amiga_convert.h>

#include <cmath>
#include <cstring>

namespace o3os::app {

namespace {

constexpr std::uint32_t kMaxGainQ16 = 255u * 65536u;

std::int16_t saturate(std::int64_t v) noexcept { return static_cast<std::int16_t>(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }

// x * g / 2^16, rounded to nearest (half away from zero), saturated.
std::int16_t scale(std::int16_t x, std::uint32_t gain_q16) noexcept {
  const std::int64_t p = static_cast<std::int64_t>(x) * static_cast<std::int64_t>(gain_q16);
  return saturate((p + (p < 0 ? -32768 : 32768)) / 65536);
}

// The fade's gain for the k-th frame of the range, Q24 (at most unity).
std::uint32_t fade_gain_q24(const EditSpec& s, std::uint32_t k) noexcept {
  const std::uint64_t g = static_cast<std::uint64_t>(k) * s.fade_step_q24;
  return g > (1u << 24) ? (1u << 24) : static_cast<std::uint32_t>(g);
}

std::int16_t fade(std::int16_t x, std::uint32_t gain_q24) noexcept { return saturate((static_cast<std::int64_t>(x) * static_cast<std::int64_t>(gain_q24)) / (1 << 24)); }

// A float result to int16: rounded to nearest, saturated (as Amplify's overs).
std::int16_t to_pcm(float y) noexcept {
  y = y > 32767.0f ? 32767.0f : (y < -32768.0f ? -32768.0f : y);
  return static_cast<std::int16_t>(static_cast<std::int32_t>(y + (y < 0.0f ? -0.5f : 0.5f)));
}

// log2 and 2^x without libm: the exponent from the bits plus a degree-4 polynomial of the mantissa (fitted for this file; error 1.2e-4 in log2 units, 4e-6 relative in exp2), a few cycles
// each. The compressor runs them once a frame, offline. x > 0 and normal for log2; exp2's argument is held to +-60.
float fast_log2(float x) noexcept {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &x, sizeof bits);
  const float e = static_cast<float>(static_cast<std::int32_t>(bits >> 23) - 127);
  bits = (bits & 0x007FFFFFu) | 0x3F800000u;
  float m = 0.0f;
  std::memcpy(&m, &bits, sizeof m);
  const float t = m - 1.0f;
  return e + (0.000114580f + t * (1.436874900f + t * (-0.670882600f + t * (0.312269400f + t * -0.078440680f))));
}

float fast_exp2(float v) noexcept {
  v = v > 60.0f ? 60.0f : (v < -60.0f ? -60.0f : v);
  std::int32_t i = static_cast<std::int32_t>(v);
  i = static_cast<float>(i) > v ? i - 1 : i;
  const float f = v - static_cast<float>(i);
  const float p = 1.0000035f + f * (0.6929730f + f * (0.2416044f + f * (0.0517450f + f * 0.0136703f)));
  const std::uint32_t bits = static_cast<std::uint32_t>(i + 127) << 23;
  float scale = 0.0f;
  std::memcpy(&scale, &bits, sizeof scale);
  return p * scale;
}

constexpr float kDbPerLog2 = 6.02059991f;  // 20 log10(2)
constexpr float kLog2E = 1.44269504f;      // exp(x) = 2^(x log2 e)

float flush(float v) noexcept { return v < 1.0e-15f && v > -1.0e-15f ? 0.0f : v; }  // no denormals in the states

std::int16_t filter_frame(const EditSpec& s, EditRun& run, std::int16_t in) noexcept {
  const float x = static_cast<float>(in);
  const float y = s.k[0] * x + run.z1;
  run.z1 = flush(s.k[1] * x - s.k[3] * y + run.z2);
  run.z2 = flush(s.k[2] * x - s.k[4] * y);
  return to_pcm(y);
}

std::int16_t comp_frame(const EditSpec& s, EditRun& run, std::int16_t in) noexcept {
  const float x = static_cast<float>(in) * (1.0f / 32768.0f);
  float level = x < 0.0f ? -x : x;
  if (s.rms) {
    run.ms = flush(run.ms + s.k[5] * (x * x - run.ms));
    level = std::sqrt(run.ms);
  }
  const float over = fast_log2(level + 1.0e-9f) - s.k[0];
  const float target = over > 0.0f ? over * s.k[1] : 0.0f;
  run.gr += (target > run.gr ? s.k[2] : s.k[3]) * (target - run.gr);
  run.gr = target == 0.0f && run.gr < 1.0e-6f ? 0.0f : run.gr;
  return to_pcm(x * 32768.0f * fast_exp2(s.k[4] - run.gr));
}

}  // namespace

EditSpec make_filter_spec(std::uint32_t frames, std::uint32_t a, std::uint32_t b, unsigned rate_hz, FilterType type, unsigned cutoff_hz, unsigned q_x10) noexcept {
  EditSpec s = make_spec(EditOp::Filter, frames, a, b, 100);
  const float rate = rate_hz != 0u ? static_cast<float>(rate_hz) : 44100.0f;
  float fc = static_cast<float>(cutoff_hz);
  fc = fc < 20.0f ? 20.0f : (fc > 0.45f * rate ? 0.45f * rate : fc);
  const float q = static_cast<float>(q_x10 < 5u ? 5u : (q_x10 > 150u ? 150u : q_x10)) * 0.1f;
  const float turns = fc / rate;                     // w0 / 2 pi, 0 < turns <= 0.45
  const float cw = sin_turns(turns + 0.25f);         // cos(w0), the amiga kernel's libm-free sine (amiga_convert.h)
  const float alpha = sin_turns(turns) / (2.0f * q);  // sin(w0) / 2Q
  float b0 = 0.0f;
  float b1 = 0.0f;
  float b2 = 0.0f;
  switch (type) {
    case FilterType::LowPass: b0 = b2 = (1.0f - cw) * 0.5f; b1 = 1.0f - cw; break;
    case FilterType::HighPass: b0 = b2 = (1.0f + cw) * 0.5f; b1 = -(1.0f + cw); break;
    case FilterType::BandPass: b0 = alpha; b1 = 0.0f; b2 = -alpha; break;  // constant 0 dB peak gain
  }
  const float inv = 1.0f / (1.0f + alpha);
  s.k[0] = b0 * inv;
  s.k[1] = b1 * inv;
  s.k[2] = b2 * inv;
  s.k[3] = -2.0f * cw * inv;
  s.k[4] = (1.0f - alpha) * inv;
  return s;
}

EditSpec make_comp_spec(std::uint32_t frames, std::uint32_t a, std::uint32_t b, unsigned rate_hz, int threshold_db, unsigned ratio_x10, unsigned attack_ms_x10, unsigned release_ms, int makeup_db,
                        bool auto_makeup, bool rms) noexcept {
  EditSpec s = make_spec(EditOp::Compressor, frames, a, b, 100);
  const float rate = rate_hz != 0u ? static_cast<float>(rate_hz) : 44100.0f;
  const float thr = static_cast<float>(threshold_db < -60 ? -60 : (threshold_db > 0 ? 0 : threshold_db));
  const float ratio = static_cast<float>(ratio_x10 < 11u ? 11u : (ratio_x10 > 200u ? 200u : ratio_x10)) * 0.1f;
  const float att = static_cast<float>(attack_ms_x10 < 1u ? 1u : (attack_ms_x10 > 1000u ? 1000u : attack_ms_x10)) * 0.1f;
  const float rel = static_cast<float>(release_ms < 5u ? 5u : (release_ms > 2000u ? 2000u : release_ms));
  float makeup = auto_makeup ? -thr * (1.0f - 1.0f / ratio) : static_cast<float>(makeup_db < 0 ? 0 : makeup_db);
  makeup = makeup > 24.0f ? 24.0f : makeup;
  s.rms = rms;
  s.k[0] = thr / kDbPerLog2;
  s.k[1] = 1.0f - 1.0f / ratio;
  s.k[2] = 1.0f - fast_exp2(-1000.0f / (att * rate) * kLog2E);  // 1 - exp(-x), no libm
  s.k[3] = 1.0f - fast_exp2(-1000.0f / (rel * rate) * kLog2E);
  s.k[4] = makeup / kDbPerLog2;
  s.k[5] = 1.0f - fast_exp2(-100.0f / rate * kLog2E);  // 10 ms
  return s;
}

void set_loop_warmup(EditSpec& spec, std::uint32_t frames, std::uint32_t loop_start, std::uint32_t loop_end, unsigned rate_hz) noexcept {
  spec.warm_from = 0;
  spec.warm_n = 0;
  loop_end = loop_end > frames ? frames : loop_end;
  if ((spec.op != EditOp::Filter && spec.op != EditOp::Compressor) || spec.a != loop_start || loop_start >= loop_end) {
    return;
  }
  const std::uint32_t cap = (rate_hz != 0u ? rate_hz : 44100u) / 10u * 3u;
  spec.warm_n = loop_end - loop_start < cap ? loop_end - loop_start : cap;
  spec.warm_from = loop_end - spec.warm_n;
}

unsigned edit_cost(EditOp op) noexcept { return op == EditOp::Compressor ? 8u : (op == EditOp::Filter ? 3u : 1u); }

EditSpec make_spec(EditOp op, std::uint32_t frames, std::uint32_t a, std::uint32_t b, unsigned percent) noexcept {
  EditSpec s;
  s.op = op;
  if (a > b) {
    const std::uint32_t t = a;
    a = b;
    b = t;
  }
  b = b > frames ? frames : b;
  a = a > b ? b : a;
  if (a == b) {  // no range: the whole sample
    a = 0;
    b = frames;
  }
  s.a = a;
  s.b = b;
  const std::uint64_t g = static_cast<std::uint64_t>(percent) * 65536u / 100u;
  s.gain_q16 = g > kMaxGainQ16 ? kMaxGainQ16 : static_cast<std::uint32_t>(g);
  const std::uint32_t n = b - a;
  s.fade_step_q24 = n > 1u ? static_cast<std::uint32_t>(((std::uint64_t{1} << 24) + n - 2u) / (n - 1u)) : 0u;
  return s;
}

EditSpec make_amiga_spec(std::uint32_t frames, unsigned period, bool src_amiga, bool q8, std::uint32_t src_rate_hz) noexcept {
  EditSpec s;
  s.op = EditOp::Amiga;
  s.a = 0;
  s.b = frames;
  s.period = static_cast<std::uint16_t>(period);
  s.step_q32 = amiga_step_q32(period, src_amiga, src_rate_hz);
  s.q8 = q8;
  return s;
}

std::uint32_t edit_result_frames(const EditSpec& spec, std::uint32_t frames) noexcept {
  if (spec.op == EditOp::Amiga) {
    return spec.step_q32 != 0u ? amiga_frames(frames, spec.step_q32) : 0u;
  }
  return spec.op == EditOp::Crop ? spec.b - spec.a : frames;
}

std::uint32_t scan_peak(const std::int16_t* src, std::uint32_t frames, std::uint32_t from, std::uint32_t n) noexcept {
  const std::uint32_t end = from + n > frames || from + n < from ? frames : from + n;
  std::uint32_t peak = 0;
  for (std::uint32_t i = from; i < end; ++i) {
    const std::int32_t x = src[i];
    const std::uint32_t m = static_cast<std::uint32_t>(x < 0 ? -x : x);
    peak = m > peak ? m : peak;
  }
  return peak;
}

std::uint32_t scan_peak_ms(const std::int16_t* mid, const std::int16_t* side, std::uint32_t frames, std::uint32_t from, std::uint32_t n) noexcept {
  const std::uint32_t end = from + n > frames || from + n < from ? frames : from + n;
  std::uint32_t peak = 0;
  for (std::uint32_t i = from; i < end; ++i) {
    const std::int32_t m = mid[i];
    const std::int32_t s = side[i];
    const std::uint32_t v = static_cast<std::uint32_t>((m < 0 ? -m : m) + (s < 0 ? -s : s));
    peak = v > peak ? v : peak;
  }
  return peak;
}

void scan_dc(const std::int16_t* src, std::uint32_t frames, std::uint32_t from, std::uint32_t n, DcScan& scan) noexcept {
  const std::uint32_t end = from + n > frames || from + n < from ? frames : from + n;
  std::int64_t sum = 0;
  std::int32_t lo = scan.lo;
  std::int32_t hi = scan.hi;
  for (std::uint32_t i = from; i < end; ++i) {
    const std::int32_t x = src[i];
    sum += x;
    lo = x < lo ? x : lo;
    hi = x > hi ? x : hi;
  }
  scan.sum += sum;
  scan.lo = lo;
  scan.hi = hi;
}

std::int32_t dc_offset(const DcScan& scan, std::uint32_t n, bool* held) noexcept {
  if (held != nullptr) {
    *held = false;
  }
  if (n == 0u) {
    return 0;
  }
  const std::int64_t half = static_cast<std::int64_t>(n / 2u);
  std::int64_t d = (scan.sum + (scan.sum < 0 ? -half : half)) / static_cast<std::int64_t>(n);
  const std::int64_t d_min = scan.hi - 32767;  // <= 0: x - d <= 32767 for every x
  const std::int64_t d_max = scan.lo + 32768;  // >= 0: x - d >= -32768 for every x
  const std::int64_t mean = d;
  d = d < d_min ? d_min : (d > d_max ? d_max : d);
  if (held != nullptr) {
    *held = d != mean;
  }
  return static_cast<std::int32_t>(d);
}

std::uint32_t normalise_gain_q16(std::uint32_t peak) noexcept {
  if (peak == 0u) {
    return 65536u;
  }
  const std::uint64_t g = static_cast<std::uint64_t>(32767u) * 65536u / peak;
  return g < 65536u ? 65536u : (g > kMaxGainQ16 ? kMaxGainQ16 : static_cast<std::uint32_t>(g));
}

void edit_warm(const EditSpec& spec, const std::int16_t* src, std::uint32_t frames, std::uint32_t from, std::uint32_t n, EditRun* run) noexcept {
  if (run == nullptr || from >= spec.warm_n || spec.warm_from >= frames) {
    return;
  }
  n = n > spec.warm_n - from ? spec.warm_n - from : n;
  n = n > frames - spec.warm_from - from ? frames - spec.warm_from - from : n;
  for (std::uint32_t k = 0; k < n; ++k) {
    const std::int16_t x = src[spec.warm_from + from + k];
    if (spec.op == EditOp::Filter) {
      (void)filter_frame(spec, *run, x);
    } else if (spec.op == EditOp::Compressor) {
      (void)comp_frame(spec, *run, x);
    }
  }
}

void edit_chunk(const EditSpec& spec, const std::int16_t* src, std::uint32_t frames, std::int16_t* dst, std::uint32_t from, std::uint32_t n, EditRun* run) noexcept {
  EditRun fresh;
  run = run != nullptr ? run : &fresh;
  const std::uint32_t total = edit_result_frames(spec, frames);
  if (from >= total || spec.op == EditOp::Amiga) {  // (Amiga: the job's, it has the kernel)
    return;
  }
  n = n > total - from ? total - from : n;
  for (std::uint32_t k = 0; k < n; ++k) {
    const std::uint32_t i = from + k;  // the result's frame
    if (spec.op == EditOp::Crop) {
      dst[k] = src[spec.a + i];
      continue;
    }
    if (i < spec.a || i >= spec.b) {
      dst[k] = src[i];
      continue;
    }
    switch (spec.op) {
      case EditOp::Reverse: dst[k] = src[spec.a + spec.b - 1u - i]; break;
      case EditOp::FadeIn: dst[k] = fade(src[i], fade_gain_q24(spec, i - spec.a)); break;
      case EditOp::FadeOut: dst[k] = fade(src[i], fade_gain_q24(spec, spec.b - 1u - i)); break;
      case EditOp::Normalise:
      case EditOp::Amplify: dst[k] = scale(src[i], spec.gain_q16); break;
      case EditOp::DcRemove: dst[k] = saturate(static_cast<std::int64_t>(src[i]) - spec.dc); break;
      case EditOp::Filter: dst[k] = filter_frame(spec, *run, src[i]); break;
      case EditOp::Compressor: dst[k] = comp_frame(spec, *run, src[i]); break;
      case EditOp::Crop:
      case EditOp::Amiga: break;
    }
  }
}

}  // namespace o3os::app
