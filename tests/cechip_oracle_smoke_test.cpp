// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-CECHIP C1: the oracle harness works. tracker-ce's CHIP engine (third_party/tracker-ce-chip, MIT; support/cechip_ref.*) compiled unmodified
// against the shim renders what its documentation says: 25 parameters with the stated defaults, every one of its eight models a finite, non-silent,
// repeatable note, a note off that ends in silence after Release, and the pitch the shim's noteToFreq promises. The vendored files' own SHA-256 is
// the ctest cechip_oracle_sha. Later slices (C3 to C5) compare Chip's kernels against this oracle; this file only proves the oracle is usable.
// Host only.
#include <doctest/doctest.h>

#include "support/cechip_ref.h"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

namespace {

using namespace cechip_ref;

constexpr std::size_t kBlock = 480;

// Renders `n` samples in blocks of an awkward size; appends to `out`.
void render(Voice& v, std::vector<float>& out, std::size_t n) {
  std::size_t done = 0;
  while (done < n) {
    const std::size_t len = (n - done < kBlock) ? (n - done) : kBlock;
    const std::size_t at = out.size();
    out.resize(at + len);
    v.render(out.data() + at, len);
    done += len;
  }
}

std::size_t half_second() { return static_cast<std::size_t>(sample_rate() * 0.5f); }

std::vector<float> note(int model, int midi_note) {
  Voice v;
  v.set(kModel, static_cast<float>(model));
  v.note_on(midi_note);
  std::vector<float> out;
  render(v, out, half_second());
  return out;
}

}  // namespace

TEST_CASE("cechip oracle: the original's 25 parameters, their defaults and ranges") {
  CHECK(param_count() == kParamCount);
  if (param_count() != kParamCount) {
    return;  // the cases below index every id
  }
  CHECK(std::strcmp(param_name(kModel), "Model") == 0);
  CHECK(std::strcmp(param_name(kBitcrush), "Bitcrush") == 0);
  CHECK(std::strcmp(param_name(kVolume), "Volume") == 0);
  // Fact F2 of docs/specs/OS-CECHIP.md.
  CHECK(param_default(kShape) == doctest::Approx(0.35f));
  CHECK(param_min(kPwmRate) == doctest::Approx(0.1f));
  CHECK(param_max(kPwmRate) == doctest::Approx(40.0f));
  CHECK(param_default(kPwmRate) == doctest::Approx(2.5f));
  CHECK(param_default(kPwmDepth) == doctest::Approx(0.2f));
  CHECK(param_default(kSub) == doctest::Approx(0.25f));
  CHECK(param_max(kChord) == doctest::Approx(7.0f));
  CHECK(param_default(kChord) == doctest::Approx(0.0f));
  CHECK(param_min(kCutoff) == doctest::Approx(80.0f));
  CHECK(param_max(kCutoff) == doctest::Approx(18000.0f));
  CHECK(param_default(kCutoff) == doctest::Approx(12000.0f));
  CHECK(param_default(kResonance) == doctest::Approx(0.15f));
  CHECK(param_min(kEnvAmt) == doctest::Approx(-1.0f));
  CHECK(param_default(kEnvAmt) == doctest::Approx(0.2f));
  CHECK(param_min(kBitcrush) == doctest::Approx(2.0f));
  CHECK(param_max(kBitcrush) == doctest::Approx(16.0f));
  CHECK(param_default(kBitcrush) == doctest::Approx(8.0f));
  CHECK(param_max(kDecimate) == doctest::Approx(32.0f));
  CHECK(param_default(kDecimate) == doctest::Approx(1.0f));
  CHECK(param_max(kModel) == doctest::Approx(static_cast<float>(kModelCount - 1)));
}

TEST_CASE("cechip oracle: each model renders a finite, non-silent note") {
  for (int model = 0; model < kModelCount; ++model) {
    CAPTURE(model);
    const std::vector<float> out = note(model, 60);
    float peak = 0.0f;
    bool finite = true;
    for (const float s : out) {
      finite = finite && std::isfinite(s);
      peak = std::fmax(peak, std::fabs(s));
    }
    CHECK(finite);
    CHECK(peak > 0.01f);
    CHECK(peak < 2.0f);
  }
}

TEST_CASE("cechip oracle: two runs are identical, bit for bit") {
  for (int model = 0; model < kModelCount; ++model) {
    CAPTURE(model);
    const std::vector<float> a = note(model, 57);
    const std::vector<float> b = note(model, 57);
    CHECK(a.size() == b.size());
    const bool same = a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
    CHECK(same);
  }
}

TEST_CASE("cechip oracle: a note off ends in silence after Release") {
  for (int model = 0; model < kModelCount; ++model) {
    CAPTURE(model);
    Voice v;
    v.set(kModel, static_cast<float>(model));
    v.set(kRelease, 0.05f);
    v.note_on(64);
    std::vector<float> out;
    render(v, out, half_second() / 2);
    CHECK(v.active());
    v.note_off(64);
    out.clear();
    render(v, out, half_second());  // 0.5 s against a 0.05 s release
    CHECK_FALSE(v.active());
    bool silent_tail = true;
    for (std::size_t i = out.size() - kBlock; i < out.size(); ++i) {
      silent_tail = silent_tail && out[i] == 0.0f;
    }
    CHECK(silent_tail);
    // a voice that never played, or has ended, renders exact zeros
    std::vector<float> idle;
    render(v, idle, kBlock);
    bool zeros = true;
    for (const float s : idle) {
      zeros = zeros && s == 0.0f;
    }
    CHECK(zeros);
  }
}

TEST_CASE("cechip oracle: the shim's note-to-frequency puts the 2A03 pulse at 440 Hz for note 69") {
  Voice v;
  v.set(kModel, 0.0f);
  v.set(kShape, 0.5f);
  v.set(kPwmDepth, 0.0f);
  v.set(kSub, 0.0f);
  v.set(kBitcrush, 16.0f);
  v.set(kCutoff, 18000.0f);
  v.set(kResonance, 0.0f);
  v.set(kSustain, 1.0f);
  v.note_on(69);
  std::vector<float> out;
  render(v, out, half_second());
  int rising = 0;
  for (std::size_t i = 1; i < out.size(); ++i) {
    if (out[i - 1] < 0.0f && out[i] >= 0.0f) {
      ++rising;
    }
  }
  CHECK(rising >= 219);
  CHECK(rising <= 221);  // 440 Hz x 0.5 s = 220 periods
}
