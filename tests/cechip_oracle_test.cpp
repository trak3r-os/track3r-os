// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-CECHIP C2, the dsp-port skill's gate 2: the pulse family of the Chip module (modules/chip/chip_ce.h, the very header the module is built from) against the ORIGINAL, tracker-ce's CHIP
// engine compiled unmodified (third_party/tracker-ce-chip, MIT; support/cechip_ref.*). The original is driven static as docs/specs/OS-CECHIP.md 3.6 says: Attack 0, Sustain 1, Volume 1, no
// vibrato, sweep, arpeggio or glide, the post chain off (Sub Osc 0, Cutoff 18000 with Resonance 0, Bitcrush 16, Decimate 1), so that its output is the kernel at a constant phase step.
//   - the kernel at policy Std, sample by sample: at most 1e-4 absolute over 0.5 s, at Shape 0 / 35 / 100 %, PWM Depth 0 / 20 / 100 %, PWM Rate 0.1 / 40 Hz and notes 24 / 60 / 108;
//   - the same through the whole voice (chip::Voice: the envelope, the control rate, the host's exp2) at policy Fast: an SNR bound, since the voice's pitch comes from the host's exp2 and the
//     original's from pow (a few ulps of frequency, which move an edge by a fraction of a sample over 0.5 s);
//   - Zap's pitch curve, by formula and against the original's frequency; the chord tables against the original's pitch steps (a formula test, the notes the original plays).
// Host only (the oracle is the original's C++20 code).
#include <doctest/doctest.h>

#include "support/cechip_ref.h"

#include "../modules/chip/chip_dsp.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

namespace {

namespace cr = cechip_ref;
constexpr std::size_t kFrames = 22050;  // 0.5 s at 44.1 kHz
constexpr float kRate = 44100.0f;

float exp2_host(float x) { return std::exp2(x); }
float sin_host(float x) { return std::sin(x); }

// The original, driven static, the pulse family (model 0 = 2A03, 5 = Zap, 6 = Arp Chord) with the post chain off.
void ce_static(cr::Voice& v, int model, float shape, float pwm_depth, float pwm_rate) {
  v.set(cr::kModel, static_cast<float>(model));
  v.set(cr::kShape, shape);
  v.set(cr::kPwmDepth, pwm_depth);
  v.set(cr::kPwmRate, pwm_rate);
  v.set(cr::kSub, 0.0f);
  v.set(cr::kCutoff, 18000.0f);
  v.set(cr::kResonance, 0.0f);
  v.set(cr::kBitcrush, 16.0f);
  v.set(cr::kDecimate, 1.0f);
  v.set(cr::kEnvAmt, 0.0f);
  v.set(cr::kAttack, 0.0f);
  v.set(cr::kSustain, 1.0f);
  v.set(cr::kVolume, 1.0f);
}

std::vector<float> ce_render(cr::Voice& v, std::size_t n) {
  std::vector<float> out(n);
  v.render(out.data(), n);
  return out;
}

// The original's phase step for a note: its frequency expression (noteToFreq(0, note) clamped to 16..18000 Hz) times 1 / sample rate.
float ce_dt(int note) {
  const float freq = 440.0f * std::pow(2.0f, (-69.0f + static_cast<float>(note)) / 12.0f);
  return std::clamp(freq, 16.0f, 18000.0f) * (1.0f / kRate);
}

o3m_host make_host() {
  o3m_host h{};
  h.size = sizeof(o3m_host);
  h.sample_rate = kRate;
  h.exp2_f = exp2_host;
  h.sin_f = sin_host;
  return h;
}

double snr_db(const std::vector<float>& ref, const std::vector<float>& got, std::size_t from) {
  double sig = 0.0, err = 0.0;
  for (std::size_t i = from; i < ref.size(); ++i) {
    sig += static_cast<double>(ref[i]) * static_cast<double>(ref[i]);
    const double d = static_cast<double>(ref[i]) - static_cast<double>(got[i]);
    err += d * d;
  }
  return err <= 0.0 ? 200.0 : 10.0 * std::log10(sig / err);
}

// The frequency of out[from, from + n) from its rising zero crossings (linearly interpolated): crossings - 1 periods over the time between the first and the last; 0 for fewer than 3 crossings.
double rising_hz(const std::vector<float>& out, std::size_t from, std::size_t n) {
  double first = -1.0, last = 0.0;
  int count = 0;
  for (std::size_t i = from + 1; i < from + n; ++i) {
    if (out[i - 1] < 0.0f && out[i] >= 0.0f) {
      const double t = static_cast<double>(i - 1) + static_cast<double>(-out[i - 1]) / static_cast<double>(out[i] - out[i - 1]);
      if (first < 0.0) {
        first = t;
      }
      last = t;
      ++count;
    }
  }
  return count >= 3 ? (count - 1) * static_cast<double>(kRate) / (last - first) : 0.0;
}

// The four steps of a chord the original plays at 5 Hz (a step every 8820 samples) from `out`: each step's pitch in semitones above note 60, rounded.
void chord_steps(const std::vector<float>& out, long semis[4]) {
  const std::size_t step = 8820;
  for (std::size_t k = 0; k < 4; ++k) {
    const double hz = rising_hz(out, k * step + 1500, step - 3000);
    semis[k] = hz > 0.0 ? std::lround(12.0 * std::log2(hz / (440.0 * std::pow(2.0, (60.0 - 69.0) / 12.0)))) : -999;
  }
}

}  // namespace

TEST_CASE("cechip oracle: the pulse kernel at policy Std equals the original, sample by sample (1e-4)") {
  const float shapes[] = {0.0f, 0.35f, 1.0f};
  const float depths[] = {0.0f, 0.2f, 1.0f};
  const float rates[] = {0.1f, 40.0f};
  const int notes[] = {24, 60, 108};
  const float inv_sr = 1.0f / kRate;
  float worst = 0.0f;
  for (const float shape : shapes) {
    for (const float depth : depths) {
      for (const float rate : rates) {
        for (const int note : notes) {
          CAPTURE(shape);
          CAPTURE(depth);
          CAPTURE(rate);
          CAPTURE(note);
          cr::Voice ref;
          ce_static(ref, 0, shape, depth, rate);
          ref.note_on(note);
          const std::vector<float> want = ce_render(ref, kFrames);

          const float dt = ce_dt(note), inv = 1.0f / dt;
          const float base = 0.125f + 0.75f * shape, k = 0.38f * depth, step = rate * inv_sr;
          float phase = 0.0f, pwm = 0.0f, max_err = 0.0f;
          for (std::size_t i = 0; i < kFrames; ++i) {
            const float duty = chip::ce::pwm_duty(pwm, step, base, k);
            const float got = chip::ce::pulse<chip::ce::Std>(phase, dt, inv, duty) * 1.0f * 1.0f;
            max_err = std::max(max_err, std::fabs(got - want[i]));
          }
          worst = std::max(worst, max_err);
          CHECK(max_err <= 1e-4f);
        }
      }
    }
  }
  MESSAGE("worst pulse error at Std: " << worst << " (the check is <= 1e-4; the value is 0 where both sides use the same libm)");
}

TEST_CASE("cechip oracle: the pulse kernel at policy Fast stays within 60 dB of Std") {
  const float dt = ce_dt(60), inv = 1.0f / dt;
  float pa = 0.0f, pb = 0.0f, wa = 0.0f, wb = 0.0f;
  std::vector<float> a(kFrames), b(kFrames);
  for (std::size_t i = 0; i < kFrames; ++i) {
    const float da = chip::ce::pwm_duty(wa, 2.5f / kRate, 0.3875f, 0.076f);
    const float db = chip::ce::pwm_duty(wb, 2.5f / kRate, 0.3875f, 0.076f);
    a[i] = chip::ce::pulse<chip::ce::Std>(pa, dt, inv, da);
    b[i] = chip::ce::pulse<chip::ce::Fast>(pb, dt, inv, db);
  }
  CHECK(snr_db(a, b, 0) >= 60.0);
}

TEST_CASE("cechip oracle: the whole voice (wave 4, 2A03 PULSE, at a fixed pitch) against the original") {
  const o3m_host host = make_host();
  struct Cfg {
    int note;
    float shape, depth, rate;
  };
  const Cfg cfgs[] = {{24, 0.0f, 0.0f, 0.1f}, {60, 0.35f, 0.2f, 2.5f}, {60, 1.0f, 1.0f, 40.0f}, {108, 0.5f, 0.2f, 2.5f}, {36, 0.35f, 0.0f, 2.5f}};
  for (const Cfg& c : cfgs) {
    CAPTURE(c.note);
    CAPTURE(c.shape);
    // 2A03 PULSE (wave 4) is the original's model 0
    cr::Voice ref;
    ce_static(ref, 0, c.shape, c.depth, c.rate);
    ref.note_on(c.note);
    const std::vector<float> want = ce_render(ref, kFrames);

    chip::Voice v{};
    chip::voice_init(v, &host);
    chip::voice_set_param(v, CHIP_PARAM_WAVE, 4.0f);
    chip::voice_set_param(v, CHIP_PARAM_VOLUME, 0.0f);
    chip::voice_set_param(v, CHIP_PARAM_VEL_SENS, 0.0f);
    chip::voice_set_param(v, CHIP_PARAM_ATTACK, 0.0f);
    chip::voice_set_param(v, CHIP_PARAM_DECAY, 0.0f);
    chip::voice_set_param(v, CHIP_PARAM_SUSTAIN, 100.0f);
    chip::voice_set_param(v, CHIP_PARAM_SWEEP_TIME, 0.0f);
    chip::voice_set_param(v, CHIP_PARAM_SHAPE, c.shape * 100.0f);
    chip::voice_set_param(v, CHIP_PARAM_PWM_DEPTH, c.depth * 100.0f);
    chip::voice_set_param(v, CHIP_PARAM_PWM_RATE, c.rate);
    chip::voice_note_on(v, static_cast<std::uint32_t>(c.note), 127u);
    std::vector<float> got(kFrames);
    for (std::size_t at = 0; at < kFrames; at += 64) {
      chip::voice_render(v, got.data() + at, static_cast<std::uint32_t>(std::min<std::size_t>(64, kFrames - at)));
    }
    // Chip's attack is a 1 ms ramp, not an instant: compare after it
    const double snr = snr_db(want, got, 200);
    MESSAGE("note " << c.note << " shape " << c.shape << ": " << snr << " dB");
    CHECK(snr >= 55.0);  // measured 64 to 122 dB: the host exp2 against the original's pow moves an edge by a fraction of a sample, most at note 108
  }
}

TEST_CASE("cechip oracle: Zap's pitch drop follows the formula and the original's pitch") {
  // By formula: the original adds depth z^3 - depth to the note's semitones, depth = 12 + 24 Shape, z = 1 - p, p = age / max(64, Sweep Time in samples) up to 1: 0 at the note's start, -depth
  // once the sweep has run (the pitch drops and stays).
  CHECK(chip::ce::zap_semitones(0.5f, 0, 44100.0f) == doctest::Approx(0.0f));
  CHECK(chip::ce::zap_semitones(0.5f, 44100, 44100.0f) == doctest::Approx(-24.0f));
  CHECK(chip::ce::zap_semitones(0.5f, 44100 * 3, 44100.0f) == doctest::Approx(-24.0f));
  CHECK(chip::ce::zap_semitones(0.0f, 22050, 44100.0f) == doctest::Approx(12.0f * (0.125f - 1.0f)));
  CHECK(chip::ce::zap_semitones(1.0f, 64, 10.0f) == doctest::Approx(-36.0f));  // the span is at least 64 samples

  // against the original: its frequency in a 40 ms window at 10 / 25 / 50 % of a 2 s sweep, from note 84
  cr::Voice ref;
  ce_static(ref, 5, 0.5f, 0.0f, 2.5f);
  ref.set(cr::kSweepTime, 2.0f);
  ref.note_on(84);
  const std::vector<float> out = ce_render(ref, 88200);
  const std::size_t win = 1764;
  const std::size_t centers[] = {8820, 22050, 44100};
  for (const std::size_t c : centers) {
    const double hz = rising_hz(out, c - win / 2, win);
    const double drop = static_cast<double>(chip::ce::zap_semitones(0.5f, static_cast<std::uint32_t>(c), 2.0f * kRate));
    const double want = 440.0 * std::pow(2.0, (84.0 - 69.0 + drop) / 12.0);
    CAPTURE(c);
    CHECK(hz == doctest::Approx(want).epsilon(0.02));
  }
}

TEST_CASE("cechip oracle: the chord tables equal the original's pitch steps") {
  // The original, a pulse at note 60 with Chord c: the pitch of each of its four steps, in semitones above the note.
  for (int c = 0; c < 8; ++c) {
    CAPTURE(c);
    cr::Voice ref;
    ce_static(ref, 0, 0.5f, 0.0f, 2.5f);
    ref.set(cr::kChord, static_cast<float>(c));
    ref.set(cr::kArpSpeed, 5.0f);
    ref.note_on(60);
    long got[4];
    chord_steps(ce_render(ref, 8820 * 4 + 100), got);
    for (std::size_t k = 0; k < 4; ++k) {
      CAPTURE(k);
      CHECK(got[k] == chip::ce::kChords[c][k]);
    }
  }
  // the ARP CHORD model (the original's model 6) plays Major while Chord is Off
  cr::Voice ref;
  ce_static(ref, 6, 0.5f, 0.0f, 2.5f);
  ref.set(cr::kArpSpeed, 5.0f);
  ref.note_on(60);
  long got[4];
  chord_steps(ce_render(ref, 8820 * 4 + 100), got);
  for (std::size_t k = 0; k < 4; ++k) {
    CAPTURE(k);
    CHECK(got[k] == chip::ce::kChords[chip::ce::kChordMajor][k]);
  }
}

TEST_CASE("cechip oracle: a legato slide on Zap after its drop keeps the pitch (the slide's control update is the wave's path, OS-CECHIP C2 fold)") {
  const o3m_host host = make_host();
  chip::Voice v{};
  chip::voice_init(v, &host);
  chip::voice_set_param(v, CHIP_PARAM_WAVE, 9.0f);  // ZAP/LASER
  chip::voice_set_param(v, CHIP_PARAM_VOLUME, 0.0f);
  chip::voice_set_param(v, CHIP_PARAM_ATTACK, 0.0f);
  chip::voice_set_param(v, CHIP_PARAM_DECAY, 0.0f);
  chip::voice_set_param(v, CHIP_PARAM_SUSTAIN, 100.0f);
  chip::voice_set_param(v, CHIP_PARAM_SWEEP_TIME, 100.0f);  // the drop lasts 100 ms: over long before the slide
  chip::voice_note_on(v, 72u, 127u);
  std::vector<float> out(20000);
  for (std::size_t at = 0; at < out.size(); at += 64) {
    chip::voice_render(v, out.data() + at, static_cast<std::uint32_t>(std::min<std::size_t>(64, out.size() - at)));
  }
  const float before = v.inc;  // the dropped pitch: note 72 less 12 + 24 x 0.35 semitones
  chip::voice_note_on(v, 72u, 127u, true);  // a RAMP note over the sounding one: the host's Slide / Glide, the same note here
  // right after the slide, until the next control update, the phase step is the same: with the waves 0..3 control it was the undropped note (3.3 times higher)
  CHECK(v.inc == doctest::Approx(before).epsilon(0.001));
  float next[32];
  chip::voice_render(v, next, 32);
  CHECK(v.inc == doctest::Approx(before).epsilon(0.001));
}

// ---- OS-CECHIP C3: the post chain (sub oscillator, decimation, bitcrush, resonant lowpass) -----------------------------------------------------------------------------------------

namespace {

// The original, static (as ce_static: Attack 0, Sustain 1, Volume 1, no vibrato, sweep, arpeggio or glide), model 0 (2A03 PULSE), note 60, Shape 35 %, PWM Depth 20 %, PWM Rate 2.5 Hz, with the post chain
// parameters given; its output is then the kernel and the post chain alone at a constant phase step and a constant envelope of 1.
struct Post {
  float sub = 0.0f, cutoff = 18000.0f, resonance = 0.0f, bits = 16.0f, env_amt = 0.0f;
  int decimate = 1;
};

std::vector<float> ce_post(const Post& p, int note) {
  cr::Voice ref;
  ce_static(ref, 0, 0.35f, 0.2f, 2.5f);
  ref.set(cr::kSub, p.sub);
  ref.set(cr::kCutoff, p.cutoff);
  ref.set(cr::kResonance, p.resonance);
  ref.set(cr::kBitcrush, p.bits);
  ref.set(cr::kDecimate, static_cast<float>(p.decimate));
  ref.set(cr::kEnvAmt, p.env_amt);
  ref.note_on(note);
  return ce_render(ref, kFrames);
}

// The same through chip_ce.h at policy Std, the original's order: kernel, sub, decimation and crush, lowpass, times an envelope of 1 and a gain of 1.
std::vector<float> our_post(const Post& p, int note) {
  using namespace chip::ce;
  const float inv_sr = 1.0f / kRate;
  const float dt = ce_dt(note), inv = 1.0f / dt;
  const float base = 0.125f + 0.75f * 0.35f, k = 0.38f * 0.2f, step = 2.5f * inv_sr;
  const bool crush_on = p.bits < 15.5f;
  const float scale = crush_on ? Std::pow2(nullptr, p.bits - 1.0f) : 32768.0f, inv_scale = 1.0f / scale;
  const float q = std::clamp(1.0f - 0.85f * p.resonance, 0.12f, 1.0f);
  const float f = svf_coef<Std>(nullptr, p.cutoff, p.env_amt, 1.0f, p.resonance, inv_sr);
  chip::Post post{};
  float phase = 0.0f, pwm = 0.0f;
  std::vector<float> out(kFrames);
  for (std::size_t i = 0; i < kFrames; ++i) {
    const float duty = pwm_duty(pwm, step, base, k);
    float x = pulse<Std>(phase, dt, inv, duty);
    sub_advance(post.sub_ph, dt);
    if (p.sub > 0.001f) {
      x = sub_mix<Std>(x, post.sub_ph, p.sub);
    }
    x = hold_crush<Std>(post, x, static_cast<std::uint32_t>(p.decimate), crush_on, scale, inv_scale);
    if (f != 0.0f) {
      x = svf_step(post, x, f, q);
    }
    out[i] = x * 1.0f * 1.0f;
  }
  return out;
}

float max_abs_diff(const std::vector<float>& a, const std::vector<float>& b) {
  float m = 0.0f;
  for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
    m = std::max(m, std::fabs(a[i] - b[i]));
  }
  return m;
}

}  // namespace

TEST_CASE("cechip oracle: the post chain at policy Std equals the original (sub, decimation, crush, lowpass, envelope amount), 1e-4") {
  std::vector<Post> cases;
  for (const float sub : {0.0f, 0.25f, 1.0f}) {
    Post p;
    p.sub = sub;
    cases.push_back(p);
  }
  for (const int dec : {1, 2, 7, 32}) {
    Post p;
    p.decimate = dec;
    cases.push_back(p);
  }
  for (const float bits : {2.0f, 8.0f, 15.4f, 15.6f, 16.0f}) {
    Post p;
    p.bits = bits;
    cases.push_back(p);
  }
  for (const float cutoff : {80.0f, 1000.0f, 17499.0f, 17500.0f}) {
    for (const float res : {0.0f, 0.02f, 1.0f}) {
      Post p;
      p.cutoff = cutoff;
      p.resonance = res;
      cases.push_back(p);
    }
  }
  for (const float amt : {-1.0f, 0.0f, 1.0f}) {
    Post p;
    p.cutoff = 1000.0f;
    p.resonance = 0.3f;
    p.env_amt = amt;
    cases.push_back(p);
  }
  Post all;  // the whole chain at once
  all.sub = 0.5f;
  all.decimate = 3;
  all.bits = 6.0f;
  all.cutoff = 2500.0f;
  all.resonance = 0.6f;
  all.env_amt = -0.4f;
  cases.push_back(all);
  float worst = 0.0f;
  for (const Post& p : cases) {
    for (const int note : {36, 60, 96}) {
      CAPTURE(p.sub);
      CAPTURE(p.decimate);
      CAPTURE(p.bits);
      CAPTURE(p.cutoff);
      CAPTURE(p.resonance);
      CAPTURE(p.env_amt);
      CAPTURE(note);
      const float err = max_abs_diff(ce_post(p, note), our_post(p, note));
      worst = std::max(worst, err);
      CHECK(err <= 1e-4f);
    }
  }
  MESSAGE("worst post-chain error at Std: " << worst << " (the check is <= 1e-4; the value is 0 where both sides use the same libm)");
}

TEST_CASE("cechip oracle: the post chain through the whole voice (Fast, the host's maths) against the original") {
  const o3m_host host = make_host();
  struct Cfg {
    std::string name;
    float sub, cutoff, res, bits, env_amt;
    int dec;
  };
  const Cfg cfgs[] = {
      {"sub", 0.25f, 18000.0f, 0.0f, 16.0f, 0.0f, 1},
      {"decimate 7", 0.0f, 18000.0f, 0.0f, 16.0f, 0.0f, 7},
      {"crush 8", 0.0f, 18000.0f, 0.0f, 8.0f, 0.0f, 1},
      {"lowpass 1000 res 30", 0.0f, 1000.0f, 0.3f, 16.0f, 0.0f, 1},
      {"lowpass env amt 50", 0.0f, 2000.0f, 0.3f, 16.0f, 0.5f, 1},
      {"all", 0.5f, 2500.0f, 0.6f, 6.0f, -0.4f, 3},
  };
  for (const Cfg& c : cfgs) {
    CAPTURE(c.name);
    Post p;
    p.sub = c.sub;
    p.cutoff = c.cutoff;
    p.resonance = c.res;
    p.bits = c.bits;
    p.env_amt = c.env_amt;
    p.decimate = c.dec;
    const std::vector<float> want = ce_post(p, 60);

    chip::Voice v{};
    chip::voice_init(v, &host);
    chip::voice_set_param(v, CHIP_PARAM_WAVE, 4.0f);
    chip::voice_set_param(v, CHIP_PARAM_VOLUME, 0.0f);
    chip::voice_set_param(v, CHIP_PARAM_VEL_SENS, 0.0f);
    chip::voice_set_param(v, CHIP_PARAM_ATTACK, 0.0f);
    chip::voice_set_param(v, CHIP_PARAM_DECAY, 0.0f);
    chip::voice_set_param(v, CHIP_PARAM_SUSTAIN, 100.0f);
    chip::voice_set_param(v, CHIP_PARAM_SWEEP_TIME, 0.0f);
    chip::voice_set_param(v, CHIP_PARAM_SHAPE, 35.0f);
    chip::voice_set_param(v, CHIP_PARAM_PWM_DEPTH, 20.0f);
    chip::voice_set_param(v, CHIP_PARAM_PWM_RATE, 2.5f);
    chip::voice_set_param(v, CHIP_PARAM_SUB_OSC, c.sub * 100.0f);
    chip::voice_set_param(v, CHIP_PARAM_CUTOFF, c.cutoff);
    chip::voice_set_param(v, CHIP_PARAM_RESONANCE, c.res * 100.0f);
    chip::voice_set_param(v, CHIP_PARAM_BITCRUSH, c.bits);
    chip::voice_set_param(v, CHIP_PARAM_ENV_AMT, c.env_amt * 100.0f);
    chip::voice_set_param(v, CHIP_PARAM_DECIMATE, static_cast<float>(c.dec));
    chip::voice_note_on(v, 60u, 127u);
    std::vector<float> got(kFrames);
    for (std::size_t at = 0; at < kFrames; at += 64) {
      chip::voice_render(v, got.data() + at, static_cast<std::uint32_t>(std::min<std::size_t>(64, kFrames - at)));
    }
    // Chip's attack is a 1 ms ramp and its lowpass takes its coefficient from the envelope every 32 samples: compare after both have settled
    const double snr = snr_db(want, got, 1500);
    MESSAGE(c.name << ": " << snr << " dB");
    CHECK(snr >= 55.0);  // measured 71 to 133 dB
  }
}

TEST_CASE("cechip oracle: waves 0..3 with only Sub Osc on are today's wave plus the original's sub mix (formula)") {
  const o3m_host host = make_host();
  for (std::uint32_t w = 0; w < 4u; ++w) {
    for (const float sub : {0.25f, 1.0f}) {
      CAPTURE(w);
      CAPTURE(sub);
      chip::Voice a{}, b{};
      for (chip::Voice* v : {&a, &b}) {
        chip::voice_init(*v, &host);
        chip::voice_set_param(*v, CHIP_PARAM_WAVE, static_cast<float>(w));
        chip::voice_set_param(*v, CHIP_PARAM_VOLUME, 0.0f);
        chip::voice_set_param(*v, CHIP_PARAM_VEL_SENS, 0.0f);
        chip::voice_set_param(*v, CHIP_PARAM_ATTACK, 0.0f);
        chip::voice_set_param(*v, CHIP_PARAM_DECAY, 0.0f);
        chip::voice_set_param(*v, CHIP_PARAM_SUSTAIN, 100.0f);
      }
      chip::voice_set_param(b, CHIP_PARAM_SUB_OSC, sub * 100.0f);
      CHECK(a.path == w);  // the defaults: today's loops
      CHECK(b.path == 4u);
      chip::voice_note_on(a, 57u, 127u);
      chip::voice_note_on(b, 57u, 127u);
      std::vector<float> oa(4410), ob(4410);
      for (std::size_t at = 0; at < oa.size(); at += 64) {
        const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::size_t>(64, oa.size() - at));
        chip::voice_render(a, oa.data() + at, n);
        chip::voice_render(b, ob.data() + at, n);
      }
      const float dt = b.inc, gain = b.gain;
      float ph = 0.0f, worst = 0.0f;
      for (std::size_t i = 0; i < oa.size(); ++i) {
        chip::ce::sub_advance(ph, dt);
        if (i < 100) {
          continue;  // the 1 ms attack ramp: the level is not 1 yet
        }
        const float want = oa[i] * (1.0f - 0.35f * sub) + gain * chip::ce::sub_step<chip::ce::Std>(ph) * (0.75f * sub);
        worst = std::max(worst, std::fabs(want - ob[i]));
      }
      CHECK(worst <= 1e-5f);
    }
  }
}

TEST_CASE("cechip oracle: the lowpass stays finite and bounded at Resonance 100 % for ten seconds, on a wave of each path") {
  const o3m_host host = make_host();
  for (const std::uint32_t wave : {0u, 3u, 4u, 10u}) {
    for (const float cutoff : {80.0f, 1000.0f, 16000.0f}) {
      for (const float amt : {-100.0f, 100.0f}) {
        CAPTURE(wave);
        CAPTURE(cutoff);
        CAPTURE(amt);
        chip::Voice v{};
        chip::voice_init(v, &host);
        chip::voice_set_param(v, CHIP_PARAM_WAVE, static_cast<float>(wave));
        chip::voice_set_param(v, CHIP_PARAM_VOLUME, 6.0f);
        chip::voice_set_param(v, CHIP_PARAM_SUSTAIN, 100.0f);
        chip::voice_set_param(v, CHIP_PARAM_CUTOFF, cutoff);
        chip::voice_set_param(v, CHIP_PARAM_RESONANCE, 100.0f);
        chip::voice_set_param(v, CHIP_PARAM_ENV_AMT, amt);
        chip::voice_note_on(v, 48u, 127u);
        float buf[64];
        bool finite = true;
        float peak = 0.0f;
        for (std::size_t at = 0; at < 441000; at += 64) {
          chip::voice_render(v, buf, 64);
          for (const float s : buf) {
            finite = finite && std::isfinite(s);
            peak = std::max(peak, std::fabs(s));
          }
        }
        CHECK(finite);
        CHECK(peak <= 6.0f);  // the lowpass's states are held to +-2; the level is at most 2 x 2 (+6 dB)
      }
    }
  }
}

// ---- OS-CECHIP C4 and C5: SID SAW/TRI, NOISE/LFSR, SUPER SQR, FM BELL, CHIP DRUM -------------------------------------------------------------------------------------------------
// The original's models 1 (SID), 2 (Noise), 4 (Super Square), 3 (FM Bell) and 7 (Chip Drum), driven static as the pulse above (3.6): the kernels of chip_ce.h at policy Std, sample by sample.
// The kernels without a quantiser (SID, Noise, Super Square) are held to 1e-4 absolute. Where a quantiser is in the path (FM's 6-bit carrier, the drum's stepped body) at most 0.1 % of the samples may
// be one step off, with an SNR of at least 80 dB (a 1-ulp difference of an argument can move a sample across a rounding edge).

namespace {

// The original's frequency for a note, in Hz (the clamp to 16..18000 included): the drum's expression multiplies it by 1 / sample rate after its pitch factor.
float ce_freq(int note) { return std::clamp(440.0f * std::pow(2.0f, (-69.0f + static_cast<float>(note)) / 12.0f), 16.0f, 18000.0f); }

struct Match {
  float worst = 0.0f;       // the largest absolute difference
  std::size_t off = 0;      // samples that differ by more than 1e-4
  float worst_off = 0.0f;   // the largest of those
  double snr = 200.0;
};

Match match(const std::vector<float>& want, const std::vector<float>& got, std::size_t from = 0, std::size_t to = static_cast<std::size_t>(-1)) {
  Match m;
  to = std::min({to, want.size(), got.size()});
  for (std::size_t i = from; i < to; ++i) {
    const float d = std::fabs(want[i] - got[i]);
    m.worst = std::max(m.worst, d);
    if (d > 1e-4f) {
      ++m.off;
      m.worst_off = std::max(m.worst_off, d);
    }
  }
  std::vector<float> w(want.begin() + static_cast<std::ptrdiff_t>(from), want.begin() + static_cast<std::ptrdiff_t>(to));
  std::vector<float> g(got.begin() + static_cast<std::ptrdiff_t>(from), got.begin() + static_cast<std::ptrdiff_t>(to));
  m.snr = snr_db(w, g, 0);
  return m;
}

// One of the original's models, static, with the post chain off: its output for `frames` samples at `note`.
std::vector<float> ce_model(int model, float shape, float depth, float rate, int note, std::size_t frames) {
  cr::Voice ref;
  ce_static(ref, model, shape, depth, rate);
  ref.note_on(note);
  return ce_render(ref, frames);
}

// The same models through chip_ce.h at policy P, in the original's order: the PWM LFO, the kernel, times an envelope of 1 and a gain of 1.
template <class P>
std::vector<float> our_sid(float shape, float depth, float rate, int note, std::size_t frames) {
  using namespace chip::ce;
  const float dt = ce_dt(note), inv = 1.0f / dt, base = 0.125f + 0.75f * shape, k = 0.38f * depth, step = rate * (1.0f / kRate);
  float phase = 0.0f, pwm = 0.0f, sub = 0.0f;
  std::vector<float> out(frames);
  for (std::size_t i = 0; i < frames; ++i) {
    const float duty = pwm_duty(pwm, step, base, k);
    out[i] = sid<P>(phase, sub, dt, inv, duty) * 1.0f * 1.0f;
    sub_advance(sub, dt);  // the post chain's step, after the model
  }
  return out;
}

std::vector<float> our_noise(float shape, float depth, int note, std::size_t frames) {
  using namespace chip::ce;
  const float dt = ce_dt(note), rate = 2.0f + 14.0f * (1.0f - depth);
  chip::Osc o{};
  o.lfsr = kLfsrSeed;
  std::vector<float> out(frames);
  for (std::size_t i = 0; i < frames; ++i) {
    out[i] = noise(o, dt, rate, shape >= 0.5f) * 1.0f * 1.0f;
  }
  return out;
}

template <class P>
std::vector<float> our_super(float shape, float depth, float rate, int note, std::size_t frames) {
  using namespace chip::ce;
  const float dt = ce_dt(note), inv = 1.0f / dt, base = 0.125f + 0.75f * shape, k = 0.38f * depth, step = rate * (1.0f / kRate);
  const float spread = 0.002f + 0.018f * shape;
  const float dt2 = dt * (1.0f + spread), dt3 = dt * (1.0f - spread);
  chip::Osc o{};
  float phase = 0.0f, pwm = 0.0f;
  std::vector<float> out(frames);
  for (std::size_t i = 0; i < frames; ++i) {
    const float duty = pwm_duty(pwm, step, base, k);
    out[i] = super_square<P>(o, phase, dt, inv, dt2, 1.0f / dt2, dt3, 1.0f / dt3, duty) * 1.0f * 1.0f;
  }
  return out;
}

// `env`: the envelope's level at each sample (null: 1).
template <class P>
std::vector<float> our_fm(float shape, float depth, float rate, int note, std::size_t frames, const float* env = nullptr) {
  using namespace chip::ce;
  const float dt = ce_dt(note), step = rate * (1.0f / kRate);
  const float ratio = fm_ratio(shape), index_k = 0.4f + 3.5f * (0.35f + 0.65f * depth);
  chip::Osc o{};
  float phase = 0.0f, pwm = 0.0f;
  std::vector<float> out(frames);
  for (std::size_t i = 0; i < frames; ++i) {
    const float e = env != nullptr ? env[i] : 1.0f;
    const float lfo = pwm_lfo(pwm, step);
    out[i] = fm_bell<P>(o, phase, dt, ratio, index_k, e, lfo) * e * 1.0f;
  }
  return out;
}

// The drum at policy P: Std takes the original's arguments (its frequency in Hz and 1 / sample rate, tau = 0.08 sr); Fast takes the phase step and 1.
template <class P>
std::vector<float> our_drum(float shape, int note, std::size_t frames) {
  using namespace chip::ce;
  const float freq = ce_freq(note), inv_sr = 1.0f / kRate, dt = freq * inv_sr, tau = 0.08f * kRate;
  const bool std_args = std::is_same<P, Std>::value;
  chip::Osc o{};
  o.lfsr = kLfsrSeed;
  o.drum_d = 1.0f;
  o.drum_k = std::exp2(-1.0f / tau * 1.44269504f);
  const o3m_host host = make_host();
  float phase = 0.0f;
  std::vector<float> out(frames);
  for (std::size_t i = 0; i < frames; ++i) {
    if (!std_args && (i % 32u) == 0u) {
      drum_anchor(o, static_cast<std::uint32_t>(i), inv_sr, &host);  // the module's control point, every 32 samples
    }
    const bool wrapped = advance_phase(phase, dt);
    out[i] = drum<P>(o, wrapped, static_cast<std::uint32_t>(i), std_args ? freq : dt, std_args ? inv_sr : 1.0f, shape, tau) * 1.0f * 1.0f;
  }
  return out;
}

}  // namespace

TEST_CASE("cechip oracle: SID SAW/TRI at policy Std equals the original (Shape 0/49/50/100, PWM Depth 0/100, notes 0/36/60/96/127), 1e-4") {
  float worst = 0.0f;
  for (const float shape : {0.0f, 0.49f, 0.5f, 1.0f}) {
    for (const float depth : {0.0f, 1.0f}) {
      for (const int note : {0, 36, 60, 96, 127}) {
        CAPTURE(shape);
        CAPTURE(depth);
        CAPTURE(note);
        const Match m = match(ce_model(1, shape, depth, 2.5f, note, kFrames), our_sid<chip::ce::Std>(shape, depth, 2.5f, note, kFrames));
        worst = std::max(worst, m.worst);
        CHECK(m.worst <= 1e-4f);
      }
    }
  }
  // the PWM's rate moves the blend of the saw and the triangle: one case at 40 Hz
  CHECK(match(ce_model(1, 0.35f, 1.0f, 40.0f, 60, kFrames), our_sid<chip::ce::Std>(0.35f, 1.0f, 40.0f, 60, kFrames)).worst <= 1e-4f);
  MESSAGE("worst SID error at Std: " << worst);
}

TEST_CASE("cechip oracle: NOISE/LFSR at policy Std equals the original (Shape 0/49/50/100, PWM Depth 0/100, notes 0/36/60/96/127), and a sample never takes more than 7 register steps") {
  float worst = 0.0f;
  for (const float shape : {0.0f, 0.49f, 0.5f, 1.0f}) {
    for (const float depth : {0.0f, 0.2f, 1.0f}) {
      for (const int note : {0, 36, 60, 96, 127}) {
        CAPTURE(shape);
        CAPTURE(depth);
        CAPTURE(note);
        const Match m = match(ce_model(2, shape, depth, 2.5f, note, kFrames), our_noise(shape, depth, note, kFrames));
        worst = std::max(worst, m.worst);
        CHECK(m.worst <= 1e-4f);
      }
    }
  }
  MESSAGE("worst noise error at Std: " << worst);
  // the bound: at the clamp (18000 Hz) and PWM Depth 0 the clock gains 0.408 x 16 = 6.53 of a step a sample; with the residue below one, a sample takes at most 7 steps. Every note, the whole depth range, and
  // the kernel's own assertion (CHIP_CE_ASSERT traps in the host build when the bound ended the loop).
  int most = 0;
  std::vector<float> dts;
  for (int note = 0; note < 128; ++note) {
    dts.push_back(ce_dt(note));
  }
  dts.push_back(chip::ce::kMaxHz * (1.0f / kRate));  // the clamp: what control() gives for a note above 135 (transposition, arpeggio, sweep, vibrato)
  for (std::size_t n = 0; n < dts.size(); ++n) {
    for (const float depth : {0.0f, 0.5f, 1.0f}) {
      const float dt = dts[n], rate = 2.0f + 14.0f * (1.0f - depth);
      chip::Osc o{};
      o.lfsr = chip::ce::kLfsrSeed;
      for (int i = 0; i < 2000; ++i) {
        most = std::max(most, static_cast<int>(o.lfsr_ph + dt * rate));  // the steps the loop is about to take
        (void)chip::ce::noise(o, dt, rate, (n & 1u) != 0u);
      }
    }
  }
  CHECK(most <= 7);
  CHECK(most == 7);  // and the bound is the real one: at the clamp a sample takes 7 steps now and then
  // the register: reseeded to 0x7FFF at a note, the original's taps (bit 6 short, bit 1 long), 15 bits
  CHECK(chip::lfsr_step(0x7FFFu, false) == 0x3FFFu);
  CHECK(chip::lfsr_step(0x0001u, false) == 0x4000u);
  CHECK(chip::lfsr_step(0x0041u, true) == 0x0020u);
  CHECK(chip::lfsr_step(0x0001u, true) == 0x4000u);
}

TEST_CASE("cechip oracle: SUPER SQR at policy Std equals the original (Shape 0/49/50/100, PWM Depth 0/100, notes 0/36/60/96/127), 1e-4") {
  float worst = 0.0f;
  for (const float shape : {0.0f, 0.49f, 0.5f, 1.0f}) {
    for (const float depth : {0.0f, 1.0f}) {
      for (const int note : {0, 36, 60, 96, 127}) {
        CAPTURE(shape);
        CAPTURE(depth);
        CAPTURE(note);
        const Match m = match(ce_model(4, shape, depth, 2.5f, note, kFrames), our_super<chip::ce::Std>(shape, depth, 2.5f, note, kFrames));
        worst = std::max(worst, m.worst);
        CHECK(m.worst <= 1e-4f);
      }
    }
  }
  MESSAGE("worst Super Square error at Std: " << worst);
}

TEST_CASE("cechip oracle: SID, SUPER SQR at policy Fast stay within 60 dB of Std") {
  for (const float shape : {0.0f, 0.35f, 1.0f}) {
    CAPTURE(shape);
    const double sid = snr_db(our_sid<chip::ce::Std>(shape, 0.2f, 2.5f, 60, kFrames), our_sid<chip::ce::Fast>(shape, 0.2f, 2.5f, 60, kFrames), 0);
    const double sup = snr_db(our_super<chip::ce::Std>(shape, 0.2f, 2.5f, 60, kFrames), our_super<chip::ce::Fast>(shape, 0.2f, 2.5f, 60, kFrames), 0);
    MESSAGE("shape " << shape << ": SID " << sid << " dB, Super Square " << sup << " dB");
    CHECK(sid >= 60.0);
    CHECK(sup >= 60.0);
  }
}

TEST_CASE("cechip oracle: SID, NOISE/LFSR and SUPER SQR through the whole voice (Fast, the host's maths) against the original") {
  const o3m_host host = make_host();
  struct Cfg {
    std::uint32_t wave;
    int model;
    int note;
    float shape, depth;
  };
  const Cfg cfgs[] = {{5, 1, 60, 0.35f, 0.2f}, {5, 1, 36, 1.0f, 1.0f}, {6, 2, 60, 0.35f, 0.2f}, {6, 2, 48, 0.8f, 0.0f}, {8, 4, 60, 0.35f, 0.2f}, {8, 4, 72, 1.0f, 0.0f}};
  for (const Cfg& c : cfgs) {
    CAPTURE(c.wave);
    CAPTURE(c.note);
    CAPTURE(c.shape);
    const std::vector<float> want = ce_model(c.model, c.shape, c.depth, 2.5f, c.note, kFrames);
    chip::Voice v{};
    chip::voice_init(v, &host);
    chip::voice_set_param(v, CHIP_PARAM_WAVE, static_cast<float>(c.wave));
    chip::voice_set_param(v, CHIP_PARAM_VOLUME, 0.0f);
    chip::voice_set_param(v, CHIP_PARAM_VEL_SENS, 0.0f);
    chip::voice_set_param(v, CHIP_PARAM_ATTACK, 0.0f);
    chip::voice_set_param(v, CHIP_PARAM_DECAY, 0.0f);
    chip::voice_set_param(v, CHIP_PARAM_SUSTAIN, 100.0f);
    chip::voice_set_param(v, CHIP_PARAM_SWEEP_TIME, 0.0f);
    chip::voice_set_param(v, CHIP_PARAM_SHAPE, c.shape * 100.0f);
    chip::voice_set_param(v, CHIP_PARAM_PWM_DEPTH, c.depth * 100.0f);
    chip::voice_set_param(v, CHIP_PARAM_PWM_RATE, 2.5f);
    chip::voice_note_on(v, static_cast<std::uint32_t>(c.note), 127u);
    std::vector<float> got(kFrames);
    for (std::size_t at = 0; at < kFrames; at += 64) {
      chip::voice_render(v, got.data() + at, static_cast<std::uint32_t>(std::min<std::size_t>(64, kFrames - at)));
    }
    const double snr = snr_db(want, got, 200);  // Chip's attack is a 1 ms ramp
    MESSAGE("wave " << c.wave << " note " << c.note << " shape " << c.shape << ": " << snr << " dB");
    CHECK(snr >= 30.0);  // the noise is a +-0.8 square that a one-sample shift of the register's clock turns into a different sequence: its bound is the loosest
  }
}

TEST_CASE("cechip oracle: the inline sine is within 2e-6 of std::sin over [-1000, 1000]") {
  double worst = 0.0;
  for (int k = -1000000; k <= 1000000; ++k) {
    const float x = static_cast<float>(k) * 1.0e-3f;
    worst = std::max(worst, std::fabs(static_cast<double>(chip::ce::sin_rad(x)) - std::sin(static_cast<double>(x))));
  }
  // and at the arguments the kernel gives it: a phase in cycles (the carrier's is within -2 .. 3) times 2 pi
  for (int k = -20000; k <= 30000; ++k) {
    const float c = static_cast<float>(k) * 1.0e-4f;
    worst = std::max(worst, std::fabs(static_cast<double>(chip::ce::Fast::sine(c)) - static_cast<double>(chip::ce::Std::sine(c))));
  }
  MESSAGE("worst sine error: " << worst);
  CHECK(worst <= 2e-6);
}

TEST_CASE("cechip oracle: FM BELL at policy Std equals the original at every ratio (Shape in steps of 1/8) and PWM Depth 0 / 100 %, the envelope at 1") {
  float worst = 0.0f;
  const float step = 0.85f / 31.0f;
  int ratios_seen = 0;
  float last_ratio = 0.0f;
  for (int k = 0; k <= 8; ++k) {
    const float shape = static_cast<float>(k) * 0.125f;
    if (chip::ce::fm_ratio(shape) != last_ratio) {
      ++ratios_seen;
      last_ratio = chip::ce::fm_ratio(shape);
    }
    for (const float depth : {0.0f, 1.0f}) {
      for (const int note : {36, 60, 96}) {
        CAPTURE(shape);
        CAPTURE(depth);
        CAPTURE(note);
        const std::vector<float> want = ce_model(3, shape, depth, 2.5f, note, kFrames);
        const Match m = match(want, our_fm<chip::ce::Std>(shape, depth, 2.5f, note, kFrames));
        worst = std::max(worst, m.worst);
        CHECK(m.off <= kFrames / 1000);
        CHECK(m.worst_off <= step * 1.05f);
        CHECK(m.snr >= 80.0);
      }
    }
  }
  CHECK(ratios_seen == 8);  // Shape k / 8 picks all eight of the original's ratios: 1, 2, 3, 3.5, 4, 5, 7, 9
  // the ratio's table at the edges of Shape x 7.99
  CHECK(chip::ce::fm_ratio(0.0f) == 1.0f);
  CHECK(chip::ce::fm_ratio(0.1253f) == 2.0f);
  CHECK(chip::ce::fm_ratio(0.4999f) == 3.5f);
  CHECK(chip::ce::fm_ratio(1.0f) == 9.0f);
  CHECK(chip::ce::fm_ratio(5.0f) == 9.0f);   // a hostile Shape stays in the table
  CHECK(chip::ce::fm_ratio(-1.0f) == 1.0f);
  MESSAGE("worst FM error at Std: " << worst);
}

TEST_CASE("cechip oracle: FM BELL's index follows the envelope (against the original at Sustain 25 / 50 / 75 %, and by formula)") {
  for (const float sus : {0.25f, 0.5f, 0.75f}) {
    CAPTURE(sus);
    // the original: Attack 0 gives 1 at the first sample, then with Decay 0 the level falls to the sustain at the second and holds
    cr::Voice ref;
    ce_static(ref, 3, 0.5f, 0.5f, 2.5f);
    ref.set(cr::kDecay, 0.0f);
    ref.set(cr::kSustain, sus);
    ref.note_on(60);
    const std::vector<float> want = ce_render(ref, kFrames);
    std::vector<float> env(kFrames, sus);
    env[0] = 1.0f;
    const std::vector<float> got = our_fm<chip::ce::Std>(0.5f, 0.5f, 2.5f, 60, kFrames, env.data());
    const Match m = match(want, got);
    CHECK(m.off <= kFrames / 1000);
    CHECK(m.snr >= 80.0);
  }
  // by formula: at an envelope of 0 the modulator has no effect, the sample is the quantised carrier alone (a sine of the phase); at 1 the index is 0.4 + 3.5 (0.35 + 0.65 depth)
  using namespace chip::ce;
  chip::Osc o{};
  float phase = 0.0f;
  const float dt = ce_dt(60);
  for (int i = 0; i < 2000; ++i) {
    const float got = fm_bell<Std>(o, phase, dt, 3.0f, 1.0f, 0.0f, 0.3f);
    CHECK(got == doctest::Approx(std::round(std::sin(phase * 2.0f * 3.14159274f) * 31.0f) / 31.0f * 0.85f).epsilon(1e-6));
  }
  CHECK(0.4f + 3.5f * (0.35f + 0.65f * 0.0f) == doctest::Approx(1.625f));
  CHECK(0.4f + 3.5f * (0.35f + 0.65f * 1.0f) == doctest::Approx(3.9f));
}

TEST_CASE("cechip oracle: FM BELL at policy Fast stays within 60 dB of Std, and through the whole voice against the original") {
  for (const float shape : {0.0f, 0.5f, 1.0f}) {
    CAPTURE(shape);
    const double snr = snr_db(our_fm<chip::ce::Std>(shape, 0.5f, 2.5f, 60, kFrames), our_fm<chip::ce::Fast>(shape, 0.5f, 2.5f, 60, kFrames), 0);
    MESSAGE("FM shape " << shape << ": Fast against Std " << snr << " dB");
    CHECK(snr >= 60.0);
  }
  const o3m_host host = make_host();
  for (const float shape : {0.0f, 0.5f, 1.0f}) {
    CAPTURE(shape);
    cr::Voice ref;
    ce_static(ref, 3, shape, 0.5f, 2.5f);
    ref.set(cr::kDecay, 0.0f);
    ref.set(cr::kSustain, 0.5f);
    ref.note_on(60);
    const std::vector<float> want = ce_render(ref, kFrames);
    chip::Voice v{};
    chip::voice_init(v, &host);
    chip::voice_set_param(v, CHIP_PARAM_WAVE, 7.0f);
    chip::voice_set_param(v, CHIP_PARAM_VOLUME, 0.0f);
    chip::voice_set_param(v, CHIP_PARAM_VEL_SENS, 0.0f);
    chip::voice_set_param(v, CHIP_PARAM_ATTACK, 0.0f);
    chip::voice_set_param(v, CHIP_PARAM_DECAY, 0.0f);
    chip::voice_set_param(v, CHIP_PARAM_SUSTAIN, 50.0f);
    chip::voice_set_param(v, CHIP_PARAM_SWEEP_TIME, 0.0f);
    chip::voice_set_param(v, CHIP_PARAM_SHAPE, shape * 100.0f);
    chip::voice_set_param(v, CHIP_PARAM_PWM_DEPTH, 50.0f);
    chip::voice_set_param(v, CHIP_PARAM_PWM_RATE, 2.5f);
    chip::voice_note_on(v, 60u, 127u);
    std::vector<float> got(kFrames);
    for (std::size_t at = 0; at < kFrames; at += 64) {
      chip::voice_render(v, got.data() + at, static_cast<std::uint32_t>(std::min<std::size_t>(64, kFrames - at)));
    }
    const double snr = snr_db(want, got, 1500);  // Chip's attack is a 1 ms ramp and its envelope steps to the sustain: compare after both
    MESSAGE("FM voice shape " << shape << ": " << snr << " dB");
    CHECK(snr >= 40.0);
  }
}

TEST_CASE("cechip oracle: CHIP DRUM at policy Std equals the original at Shape 0/50/75/76/100 over the first 180 samples and a 2 s tail") {
  const std::size_t frames = 88200;
  float worst_head = 0.0f, worst = 0.0f;
  for (const float shape : {0.0f, 0.5f, 0.75f, 0.76f, 1.0f}) {
    for (const int note : {36, 60, 96}) {
      CAPTURE(shape);
      CAPTURE(note);
      const std::vector<float> want = ce_model(7, shape, 0.2f, 2.5f, note, frames);
      const std::vector<float> got = our_drum<chip::ce::Std>(shape, note, frames);
      const Match head = match(want, got, 0, 180);
      worst_head = std::max(worst_head, head.worst);
      CHECK(head.worst <= 1e-4f);  // the first 180 samples: the noise mix is 0.7 and the pitch drop is at its steepest
      const Match all = match(want, got);
      worst = std::max(worst, all.worst);
      CHECK(all.off <= frames / 1000);
      CHECK(all.worst_off <= 0.14f);  // one step of the stepped triangle (1 / 7.5)
      CHECK(all.snr >= 80.0);
    }
  }
  MESSAGE("worst drum error at Std: head " << worst_head << ", whole " << worst);
}

TEST_CASE("cechip oracle: CHIP DRUM at policy Fast (the decay anchored every 32 samples, then the recurrence) follows exp(-age / (0.08 sr)), and stays within 40 dB of Std") {
  const float tau = 0.08f * kRate;
  const o3m_host host = make_host();
  chip::Osc o{};
  o.drum_d = 1.0f;
  o.drum_k = std::exp2(-1.0f / tau * 1.44269504f);
  float worst_rel = 0.0f;
  for (std::uint32_t age = 0; age < 30000u; ++age) {
    if (age % 32u == 0u) {
      chip::ce::drum_anchor(o, age, 1.0f / kRate, &host);
    }
    const float d = chip::ce::Fast::decay(o, age, 0.0f);
    const float want = std::exp(-static_cast<float>(age) / tau);
    worst_rel = std::max(worst_rel, std::fabs(d - want) / want);
  }
  MESSAGE("worst relative error of the decay recurrence over 30000 samples: " << worst_rel);
  CHECK(worst_rel <= 1e-5f);
  for (const float shape : {0.0f, 0.5f, 1.0f}) {
    CAPTURE(shape);
    const double snr = snr_db(our_drum<chip::ce::Std>(shape, 60, 44100), our_drum<chip::ce::Fast>(shape, 60, 44100), 0);
    MESSAGE("drum shape " << shape << ": Fast against Std " << snr << " dB");
    CHECK(snr >= 40.0);  // measured 44.5 / 55.1 / bit-exact: what is left is the stepped body's rounding edges (a 1-ulp phase step moves a dozen samples a second). Below the spec's 60 dB at Shape 0 and 50 %, and keeping Std's call (the host's exp2_f at every sample) does not reach it either (50.1 / 64.6 dB: the host's exp2 is not the original's expf), so Fast stays and this wave's bound is 40 dB (README, C6)
  }
}

TEST_CASE("cechip oracle: CHIP DRUM through the whole voice against the original, and the register is reseeded at a note") {
  const o3m_host host = make_host();
  for (const float shape : {0.0f, 0.5f, 1.0f}) {
    CAPTURE(shape);
    const std::vector<float> want = ce_model(7, shape, 0.2f, 2.5f, 48, 8820);
    chip::Voice v{};
    chip::voice_init(v, &host);
    chip::voice_set_param(v, CHIP_PARAM_WAVE, 11.0f);
    chip::voice_set_param(v, CHIP_PARAM_VOLUME, 0.0f);
    chip::voice_set_param(v, CHIP_PARAM_VEL_SENS, 0.0f);
    chip::voice_set_param(v, CHIP_PARAM_ATTACK, 0.0f);
    chip::voice_set_param(v, CHIP_PARAM_DECAY, 0.0f);
    chip::voice_set_param(v, CHIP_PARAM_SUSTAIN, 100.0f);
    chip::voice_set_param(v, CHIP_PARAM_SWEEP_TIME, 0.0f);
    chip::voice_set_param(v, CHIP_PARAM_SHAPE, shape * 100.0f);
    v.osc.lfsr = 0x1234u;  // a note puts it back to the seed
    chip::voice_note_on(v, 48u, 127u);
    CHECK(v.osc.lfsr == 0x7FFFu);
    std::vector<float> got(8820);
    for (std::size_t at = 0; at < got.size(); at += 64) {
      chip::voice_render(v, got.data() + at, static_cast<std::uint32_t>(std::min<std::size_t>(64, got.size() - at)));
    }
    const double snr = snr_db(want, got, 1500);
    MESSAGE("drum voice shape " << shape << ": " << snr << " dB (from sample 1500; the first ms is Chip's attack ramp)");
    CHECK(snr >= 20.0);
  }
}

TEST_CASE("cechip oracle: a note reseeds the register and the drum's decay, a slide (RAMP over a sounding note) leaves them running") {
  const o3m_host host = make_host();
  chip::Voice v{};
  chip::voice_init(v, &host);
  CHECK(v.osc.lfsr == chip::ce::kLfsrSeed);
  chip::voice_set_param(v, CHIP_PARAM_WAVE, 11.0f);
  chip::voice_note_on(v, 60u, 127u);
  float buf[64];
  chip::voice_render(v, buf, 64);
  chip::voice_render(v, buf, 64);
  CHECK(v.osc.lfsr != chip::ce::kLfsrSeed);
  CHECK(v.osc.drum_d < 1.0f);
  const std::uint32_t reg = v.osc.lfsr;
  const float d = v.osc.drum_d;
  chip::voice_note_on(v, 64u, 127u, true);  // the slide: the envelope, the register and the decay go on
  CHECK(v.osc.lfsr == reg);
  CHECK(v.osc.drum_d == doctest::Approx(d).epsilon(1e-5));  // re-anchored by the slide's control update, not reset to 1
  chip::voice_note_on(v, 64u, 127u);  // a note: back to the seed and to 1
  CHECK(v.osc.lfsr == chip::ce::kLfsrSeed);
  CHECK(v.osc.drum_d == 1.0f);
}
