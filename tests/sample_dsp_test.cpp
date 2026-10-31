// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-011a: the offline effects (app/sample_dsp.h) and the job that runs them (app/sample_edit_job.h): what each does to which frames, the source never touched, the same
// result however the work is cut into chunks and steps, crop's points (wave_math.h crop_points).
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3os/app/sample_dsp.h>
#include <o3os/app/sample_edit_job.h>
#include <o3os/app/wave_math.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

using namespace o3os::app;

namespace {

std::vector<std::int16_t> tone(std::uint32_t frames, int amp = 10000) {
  std::vector<std::int16_t> v(frames);
  for (std::uint32_t i = 0; i < frames; ++i) {
    v[i] = static_cast<std::int16_t>(std::lround(amp * std::sin(0.05 * i + 0.3)));
  }
  return v;
}

// The whole result in one go.
std::vector<std::int16_t> run(const EditSpec& spec, const std::vector<std::int16_t>& src) {
  std::vector<std::int16_t> out(edit_result_frames(spec, static_cast<std::uint32_t>(src.size())));
  edit_chunk(spec, src.data(), static_cast<std::uint32_t>(src.size()), out.data(), 0, static_cast<std::uint32_t>(out.size()));
  return out;
}

}  // namespace

TEST_CASE("sample dsp: make_spec orders and clamps the range, an empty one is all of it, the gain and the fade step follow") {
  EditSpec s = make_spec(EditOp::Amplify, 1000, 700, 300, 200);
  CHECK((s.a == 300u && s.b == 700u));
  CHECK(s.gain_q16 == 131072u);
  s = make_spec(EditOp::Reverse, 1000, 400, 400, 100);
  CHECK((s.a == 0u && s.b == 1000u));
  s = make_spec(EditOp::Reverse, 1000, 900, 5000, 100);
  CHECK((s.a == 900u && s.b == 1000u));
  s = make_spec(EditOp::FadeIn, 1000, 0, 101, 100);
  CHECK(s.fade_step_q24 == ((1u << 24) + 99u) / 100u);  // rounded up: the last frame reaches unity
  CHECK(make_spec(EditOp::Amplify, 10, 0, 10, 4000000).gain_q16 == 255u * 65536u);  // held to 255x
  CHECK(edit_result_frames(make_spec(EditOp::Crop, 1000, 100, 350, 100), 1000) == 250u);
  CHECK(edit_result_frames(make_spec(EditOp::Reverse, 1000, 100, 350, 100), 1000) == 1000u);
}

TEST_CASE("sample dsp: crop keeps the range, reverse mirrors it, both leave the source alone") {
  const std::vector<std::int16_t> src = tone(2000);
  const std::vector<std::int16_t> copy = src;
  const std::vector<std::int16_t> crop = run(make_spec(EditOp::Crop, 2000, 300, 800, 100), src);
  NEED(crop.size() == 500u);
  for (std::uint32_t i = 0; i < 500; ++i) {
    CHECK(crop[i] == src[300 + i]);
  }
  const std::vector<std::int16_t> rev = run(make_spec(EditOp::Reverse, 2000, 300, 800, 100), src);
  NEED(rev.size() == 2000u);
  for (std::uint32_t i = 0; i < 2000; ++i) {
    CHECK(rev[i] == (i >= 300 && i < 800 ? src[300 + 799 - i] : src[i]));
  }
  CHECK(run(make_spec(EditOp::Reverse, 2000, 300, 800, 100), rev) == src);  // twice is the identity
  CHECK(src == copy);
}

TEST_CASE("sample dsp: fades run from silence to unity and back, linear, outside the range untouched") {
  const std::vector<std::int16_t> src(1000, 20000);
  const std::vector<std::int16_t> in = run(make_spec(EditOp::FadeIn, 1000, 100, 600, 100), src);
  CHECK(in[99] == 20000);
  CHECK(in[100] == 0);   // the first frame of the fade is silent
  CHECK(in[599] == 20000);  // the last is the source's
  CHECK(in[600] == 20000);
  for (std::uint32_t i = 101; i < 600; ++i) {
    CHECK(in[i] >= in[i - 1]);
  }
  CHECK(std::abs(in[350] - 10000) < 120);  // half way: half
  const std::vector<std::int16_t> out = run(make_spec(EditOp::FadeOut, 1000, 100, 600, 100), src);
  CHECK(out[100] == 20000);
  CHECK(out[599] == 0);
  CHECK(out[600] == 20000);
  for (std::uint32_t i = 101; i < 600; ++i) {
    CHECK(out[i] <= out[i - 1]);
  }
  // A range of one frame: the fade is its gain 0 at the end it starts from.
  CHECK(run(make_spec(EditOp::FadeIn, 10, 4, 5, 100), std::vector<std::int16_t>(10, 100))[4] == 0);
}

TEST_CASE("sample dsp: amplify scales the range and saturates; normalise brings its peak to full scale and never lowers it") {
  std::vector<std::int16_t> src = tone(1000, 8000);
  src[500] = -8000;
  const std::vector<std::int16_t> amp = run(make_spec(EditOp::Amplify, 1000, 200, 800, 200), src);
  CHECK(amp[100] == src[100]);
  CHECK(amp[900] == src[900]);
  CHECK(amp[500] == -16000);
  std::vector<std::int16_t> loud(100, 20000);
  CHECK(run(make_spec(EditOp::Amplify, 100, 0, 100, 400), loud)[0] == 32767);
  loud[0] = -20000;
  CHECK(run(make_spec(EditOp::Amplify, 100, 0, 100, 400), loud)[0] == -32768);
  CHECK(run(make_spec(EditOp::Amplify, 100, 0, 100, 50), loud)[1] == 10000);
  // Normalise: the peak of the range (8000) goes to 32767.
  CHECK(scan_peak(src.data(), 1000, 0, 1000) == 8000u);
  CHECK(scan_peak(src.data(), 1000, 600, 100) < 8000u);
  CHECK(scan_peak(src.data(), 1000, 990, 500) <= 8000u);  // past the end is held to the sample
  CHECK(scan_peak(nullptr, 0, 0, 0) == 0u);
  EditSpec n = make_spec(EditOp::Normalise, 1000, 0, 1000, 100);
  n.gain_q16 = normalise_gain_q16(8000);
  const std::vector<std::int16_t> norm = run(n, src);
  CHECK(norm[500] == -32767);  // -8000 x 4.0959 rounded: full scale
  CHECK(normalise_gain_q16(32767) == 65536u);   // already full scale: unity
  CHECK(normalise_gain_q16(32768) == 65536u);   // never below unity
  CHECK(normalise_gain_q16(0) == 65536u);       // silence has nothing to scale
  CHECK(normalise_gain_q16(1) == 255u * 65536u);  // held
}

// OS-SEDFX: DC removal runs the job's way: scan the range, then write.
std::vector<std::int16_t> dc_removed(const std::vector<std::int16_t>& src, std::uint32_t a, std::uint32_t b, EditSpec* used = nullptr, bool* held = nullptr) {
  const std::uint32_t frames = static_cast<std::uint32_t>(src.size());
  std::vector<std::int16_t> out(frames);
  SampleEditJob job;
  if (!job.begin(make_spec(EditOp::DcRemove, frames, a, b, 100), src.data(), frames, out.data(), "DC")) {
    CHECK_MESSAGE(false, "begin refused");
    return out;
  }
  while (job.step(37) == o3os::core::JobStatus::Running) {
  }
  if (used != nullptr) {
    *used = job.spec();
  }
  if (held != nullptr) {
    *held = job.held();
  }
  return out;
}

double mean_of(const std::vector<std::int16_t>& v, std::uint32_t a, std::uint32_t b) {
  double sum = 0;
  for (std::uint32_t i = a; i < b; ++i) {
    sum += v[i];
  }
  return sum / (b - a);
}

TEST_CASE("sample dsp: DC removal subtracts the mean of the range, leaves the rest, and the mean goes to zero (OS-SEDFX)") {
  std::vector<std::int16_t> src = tone(4000, 6000);
  for (std::int16_t& x : src) {
    x = static_cast<std::int16_t>(x + 2500);  // a constant offset on a tone
  }
  EditSpec used;
  std::vector<std::int16_t> out = dc_removed(src, 0, 4000, &used);
  CHECK(std::abs(mean_of(out, 0, 4000)) < 1.0);
  CHECK(used.dc != 0);
  CHECK(out[10] == src[10] - used.dc);
  // A pure constant goes to silence.
  const std::vector<std::int16_t> flat(500, -1234);
  const std::vector<std::int16_t> zero = dc_removed(flat, 0, 500);
  for (const std::int16_t x : zero) {
    CHECK(x == 0);
  }
  // A selection: only [1000, 3000) is centred, the frames around it are as they were.
  out = dc_removed(src, 1000, 3000);
  CHECK(std::abs(mean_of(out, 1000, 3000)) < 1.0);
  CHECK(out[999] == src[999]);
  CHECK(out[3000] == src[3000]);
  CHECK(out[0] == src[0]);
  CHECK(out[3999] == src[3999]);
  // Silence stays silence (and the job knows there was nothing to do).
  const std::vector<std::int16_t> quiet(300, 0);
  std::vector<std::int16_t> sink(300);
  SampleEditJob job;
  NEED(job.begin(make_spec(EditOp::DcRemove, 300, 0, 300, 100), quiet.data(), 300, sink.data(), "DC"));
  while (job.step(1000) == o3os::core::JobStatus::Running) {
  }
  CHECK(job.unchanged());
  CHECK(sink == quiet);
  // The mean rounds to nearest, away from zero at the half.
  DcScan scan;
  const std::int16_t two[2] = {1, 2};  // mean 1.5
  scan_dc(two, 2, 0, 2, scan);
  CHECK(dc_offset(scan, 2) == 2);
  const std::int16_t neg[2] = {-1, -2};
  DcScan scan2;
  scan_dc(neg, 2, 0, 2, scan2);
  CHECK(dc_offset(scan2, 2) == -2);
  CHECK(dc_offset(DcScan{}, 0) == 0);
}

TEST_CASE("sample dsp: DC removal never clips: the shift is held where a frame at full scale would leave int16") {
  // A mean of +10000 with a frame at -32768: the whole shift would give -42768.
  std::vector<std::int16_t> src(100, 10000);
  src[50] = -32768;
  src[51] = 32767;
  EditSpec used;
  bool held = false;
  const std::vector<std::int16_t> out = dc_removed(src, 0, 100, &used, &held);
  CHECK(held);  // nothing moved, though the mean is not 0: the strip says NO ROOM, not NO DC OFFSET
  CHECK(used.dc == 0);  // lo + 32768 = 0: no room to subtract a positive offset, so nothing moves
  CHECK(out == src);
  // Room on one side only: -20000 mean, a frame at +32767 allows no move to the negative either.
  std::vector<std::int16_t> neg(100, -20000);
  neg[0] = 32767;
  const std::vector<std::int16_t> out2 = dc_removed(neg, 0, 100, &used, &held);
  CHECK(held);
  CHECK(used.dc == 0);
  CHECK(out2 == neg);
  // Partial: a mean near -14800 would push the frame at 20000 over full scale, so the shift is held at -12767 (hi - 32767) and the frame lands on 32767.
  std::vector<std::int16_t> mid(100, -15000);
  mid[3] = 20000;
  mid[4] = -32768;
  const std::vector<std::int16_t> out3 = dc_removed(mid, 0, 100, &used, &held);
  CHECK(held);  // partial: the strip says DC PARTLY REMOVED
  for (std::uint32_t i = 0; i < 100; ++i) {
    CHECK(out3[i] == static_cast<std::int16_t>(mid[i] - used.dc));
    CHECK(static_cast<int>(mid[i]) - used.dc <= 32767);
    CHECK(static_cast<int>(mid[i]) - used.dc >= -32768);
  }
  CHECK(used.dc == -12767);
  CHECK(out3[3] == 32767);
  // A mean that fits whole is not held; a zero mean is not held either (the strip says NO DC OFFSET).
  src = std::vector<std::int16_t>(100, 500);
  (void)dc_removed(src, 0, 100, &used, &held);
  CHECK_FALSE(held);
  CHECK(used.dc == 500);
  (void)dc_removed(std::vector<std::int16_t>(100, 0), 0, 100, &used, &held);
  CHECK_FALSE(held);
  CHECK(used.dc == 0);
}

TEST_CASE("sample dsp: DC removal in chunks of any size equals the whole") {
  std::vector<std::int16_t> src = tone(5000, 9000);
  for (std::int16_t& x : src) {
    x = static_cast<std::int16_t>(x - 1800);
  }
  EditSpec used;
  const std::vector<std::int16_t> whole = dc_removed(src, 1200, 3700, &used);
  for (const std::uint32_t chunk : {1u, 7u, 512u, 100000u}) {
    std::vector<std::int16_t> out(5000);
    for (std::uint32_t from = 0; from < out.size(); from += chunk) {
      edit_chunk(used, src.data(), 5000, out.data() + from, from, chunk);
    }
    CHECK(out == whole);
  }
}

TEST_CASE("sample dsp: the result is the same however it is cut into chunks, and a chunk past the end writes nothing") {
  const std::vector<std::int16_t> src = tone(5000);
  for (const EditOp op : {EditOp::Crop, EditOp::Reverse, EditOp::FadeIn, EditOp::FadeOut, EditOp::Amplify}) {
    const EditSpec spec = make_spec(op, 5000, 1200, 3700, 250);
    const std::vector<std::int16_t> whole = run(spec, src);
    for (const std::uint32_t chunk : {1u, 7u, 512u, 100000u}) {
      std::vector<std::int16_t> out(whole.size());
      for (std::uint32_t from = 0; from < out.size(); from += chunk) {
        edit_chunk(spec, src.data(), 5000, out.data() + from, from, chunk);  // the last chunk is held to the result's length
      }
      CHECK(out == whole);
    }
  }
  std::int16_t guard = 77;
  edit_chunk(make_spec(EditOp::Reverse, 100, 0, 100, 100), tone(100).data(), 100, &guard, 100, 5);
  CHECK(guard == 77);
}

TEST_CASE("sample edit job: a job's result equals the one-shot, in steps of any size, its progress reaches the end; normalise scans first and knows when it has nothing to do") {
  const std::vector<std::int16_t> src = tone(30000, 6000);
  for (const EditOp op : {EditOp::Crop, EditOp::Normalise, EditOp::Reverse, EditOp::FadeOut, EditOp::Amplify}) {
    EditSpec spec = make_spec(op, 30000, 5000, 25000, 300);
    std::vector<std::int16_t> out(edit_result_frames(spec, 30000));
    SampleEditJob job;
    NEED(job.begin(spec, src.data(), 30000, out.data(), "TEST"));
    CHECK(std::string(job.name()) == "EDIT TEST");
    unsigned steps = 0;
    unsigned last = 0;
    while (job.step(op == EditOp::Crop ? 1 : 50) == o3os::core::JobStatus::Running && steps < 1000000u) {
      CHECK(job.progress() >= last);
      last = job.progress();
      ++steps;
    }
    CHECK(job.progress() == o3os::core::kJobProgressMax);
    EditSpec used = spec;
    if (op == EditOp::Normalise) {
      used.gain_q16 = normalise_gain_q16(scan_peak(src.data(), 30000, 5000, 20000));
      CHECK(job.spec().gain_q16 == used.gain_q16);
      CHECK_FALSE(job.unchanged());
    }
    CHECK(out == run(used, src));
    CHECK(steps > 10u);
  }
  // Nothing to normalise: full scale already, or silence.
  std::vector<std::int16_t> full(1000, 32767);
  std::vector<std::int16_t> out(1000);
  SampleEditJob job;
  NEED(job.begin(make_spec(EditOp::Normalise, 1000, 0, 1000, 100), full.data(), 1000, out.data(), "N"));
  while (job.step(1000) == o3os::core::JobStatus::Running) {
  }
  CHECK(job.unchanged());
  // Refusals and cancel.
  CHECK_FALSE(job.begin(make_spec(EditOp::Reverse, 1000, 0, 1000, 100), nullptr, 1000, out.data(), "X"));
  CHECK_FALSE(job.begin(make_spec(EditOp::Reverse, 1000, 0, 1000, 100), full.data(), 0, out.data(), "X"));
  NEED(job.begin(make_spec(EditOp::Reverse, 1000, 0, 1000, 100), full.data(), 1000, out.data(), "X"));
  job.cancel();
  CHECK(job.step(100) == o3os::core::JobStatus::Failed);
}

// OS-FINALQA FQA-10b2 (QA-SED-002/004): the second NORMALISE of a normalised sample is the nothing case, for every peak (the first one must land on the target, not a step short of it).
TEST_CASE("sample edit job: normalising twice, the second is nothing to do, whatever the peak") {
  auto normalise = [](std::vector<std::int16_t>& pcm, bool* nothing) {
    const std::uint32_t frames = static_cast<std::uint32_t>(pcm.size());
    std::vector<std::int16_t> out(frames);
    SampleEditJob job;
    NEED(job.begin(make_spec(EditOp::Normalise, frames, 0, frames, 100), pcm.data(), frames, out.data(), "N"));
    while (job.step(1000) == o3os::core::JobStatus::Running) {
    }
    *nothing = job.unchanged();
    if (!*nothing) {
      pcm = out;
    }
  };
  for (std::int16_t peak = 129; peak < 32767; ++peak) {  // (below 129 the gain is held to 255x: one press does not reach full scale, by design)
    for (const int sign : {1, -1}) {
      std::vector<std::int16_t> pcm = {0, static_cast<std::int16_t>(sign * peak), static_cast<std::int16_t>(-sign * (peak / 2))};
      bool nothing = false;
      normalise(pcm, &nothing);
      bool again = false;
      normalise(pcm, &again);
      CHECK_MESSAGE(again, "peak ", peak, " sign ", sign);
      if (!again) {
        return;  // (one failure says it)
      }
    }
  }
  std::vector<std::int16_t> pcm = tone(30000, 6000);
  bool nothing = false;
  normalise(pcm, &nothing);
  CHECK_FALSE(nothing);
  normalise(pcm, &nothing);
  CHECK(nothing);
}

TEST_CASE("crop points: every point and slice keeps the frame it names, relative to the new start; those outside are held or dropped; the order stays valid") {
  PointsRec in;
  in.points = Points{0, 13107, 52428, 65535};  // loop 20 %..80 %
  const std::uint32_t frames = 100000;
  for (unsigned i = 0; i < 5; ++i) {
    in.slices[i] = static_cast<std::uint16_t>(i * 65535u / 5u);  // 0, 20, 40, 60, 80 %
  }
  in.slice_count = 5;
  // Crop to 30 %..70 %.
  const PointsRec out = crop_points(in, frames, 30000, 70000);
  CHECK(points_valid(out.points));
  CHECK(out.points.start == 0u);
  CHECK(out.points.end == 65535u);
  CHECK(out.points.loop_start == 0u);  // 20 % is before the new start: held to it
  CHECK(out.points.loop_end > 49000u);   // 80 % is past the end: held to it... the loop end was 80 % of 100000 = 80000 -> past 70000
  CHECK(out.points.loop_end <= 65535u);
  // Slices at 40 % and 60 % survive, at 25 % and 75 % of the new length.
  NEED(out.slice_count == 2u);
  CHECK(std::abs(static_cast<int>(out.slices[0]) - 16384) < 3);
  CHECK(std::abs(static_cast<int>(out.slices[1]) - 49151) < 3);
  // A crop to all of it changes nothing.
  const PointsRec same = crop_points(in, frames, 0, frames);
  CHECK(same.points.loop_start == in.points.loop_start);
  CHECK(same.slice_count == 5u);
  // Degenerate input comes back as it is.
  CHECK(crop_points(in, frames, 500, 500).slice_count == 5u);
  CHECK(crop_points(in, 0, 0, 10).slice_count == 5u);
}

// ---- OS-AUDIT-15: FILTER and COMPRESSOR (new Community OS behaviour) ----------------------------------------------------------------------------------------------

namespace {

constexpr unsigned kRate = 44100;

std::vector<std::int16_t> sine_hz(std::uint32_t frames, double hz, int amp) {
  std::vector<std::int16_t> v(frames);
  for (std::uint32_t i = 0; i < frames; ++i) {
    v[i] = static_cast<std::int16_t>(std::lround(amp * std::sin(6.283185307179586 * hz * i / kRate)));
  }
  return v;
}

// RMS of frames [from, to).
double rms_of(const std::vector<std::int16_t>& v, std::size_t from, std::size_t to) {
  double sum = 0.0;
  for (std::size_t i = from; i < to; ++i) {
    sum += static_cast<double>(v[i]) * v[i];
  }
  return std::sqrt(sum / static_cast<double>(to - from));
}

double db(double ratio) { return 20.0 * std::log10(ratio); }

// Frames outside [a, b) equal in both.
bool same_outside(const std::vector<std::int16_t>& x, const std::vector<std::int16_t>& y, std::size_t a, std::size_t b) {
  for (std::size_t i = 0; i < x.size(); ++i) {
    if ((i < a || i >= b) && x[i] != y[i]) {
      return false;
    }
  }
  return true;
}

bool all_zero(const std::vector<std::int16_t>& x) {
  for (const std::int16_t v : x) {
    if (v != 0) {
      return false;
    }
  }
  return true;
}

// The gain in dB of the filter at `hz`, measured on the settled second half.
double filter_gain_db(FilterType type, unsigned cutoff, unsigned q_x10, double hz, int amp = 8000) {
  const std::uint32_t n = 44100;
  const std::vector<std::int16_t> src = sine_hz(n, hz, amp);
  const std::vector<std::int16_t> out = run(make_filter_spec(n, 0, n, kRate, type, cutoff, q_x10), src);
  return db(rms_of(out, n / 2, n) / rms_of(src, n / 2, n));
}

// The analogue 2nd-order response, prewarped as the bilinear transform does: |H| for LP (hp = false) or HP of a given Q at f / fc.
double biquad_db(bool hp, double fc, double q, double f) {
  const double pi = 3.14159265358979323846;
  const double w = std::tan(pi * f / kRate) / std::tan(pi * fc / kRate);
  const double w2 = w * w;
  const double den = std::sqrt((1.0 - w2) * (1.0 - w2) + w2 / (q * q));
  return db((hp ? w2 : 1.0) / den);
}

}  // namespace

TEST_CASE("sample dsp: the low pass passes a tone below the cutoff and attenuates one above it as the 2nd-order response says (OS-AUDIT-15)") {
  CHECK(std::abs(filter_gain_db(FilterType::LowPass, 1000, 7, 100.0) - biquad_db(false, 1000, 0.7, 100.0)) < 0.3);
  CHECK(filter_gain_db(FilterType::LowPass, 1000, 7, 100.0) > -0.5);
  const double above = filter_gain_db(FilterType::LowPass, 1000, 7, 8000.0);
  CHECK(above < -35.0);
  CHECK(std::abs(above - biquad_db(false, 1000, 0.7, 8000.0)) < 0.5);
  // At the cutoff a Q of 0.707 is 3 dB down, and a higher Q rings up.
  CHECK(std::abs(filter_gain_db(FilterType::LowPass, 2000, 7, 2000.0) - biquad_db(false, 2000, 0.7, 2000.0)) < 0.3);
  CHECK(std::abs(filter_gain_db(FilterType::LowPass, 2000, 100, 2000.0, 1000) - biquad_db(false, 2000, 10.0, 2000.0)) < 0.5);  // about +20 dB (a small tone: no saturation)
}

TEST_CASE("sample dsp: the high pass is the reverse, the band pass peaks at its centre at unity (OS-AUDIT-15)") {
  CHECK(std::abs(filter_gain_db(FilterType::HighPass, 4000, 7, 12000.0) - biquad_db(true, 4000, 0.7, 12000.0)) < 0.3);
  CHECK(filter_gain_db(FilterType::HighPass, 4000, 7, 12000.0) > -0.5);
  const double below = filter_gain_db(FilterType::HighPass, 4000, 7, 500.0);
  CHECK(below < -35.0);
  CHECK(std::abs(below - biquad_db(true, 4000, 0.7, 500.0)) < 0.6);
  const double centre = filter_gain_db(FilterType::BandPass, 2000, 20, 2000.0);
  CHECK(std::abs(centre) < 0.3);
  CHECK(filter_gain_db(FilterType::BandPass, 2000, 20, 200.0) < -15.0);
  CHECK(filter_gain_db(FilterType::BandPass, 2000, 20, 15000.0) < -15.0);
}

TEST_CASE("sample dsp: the filter touches only the selection, its result is the same in chunks of any size, silence stays silent, overs saturate (OS-AUDIT-15)") {
  const std::uint32_t n = 20000;
  const std::vector<std::int16_t> src = sine_hz(n, 5000.0, 20000);
  const EditSpec spec = make_filter_spec(n, 5000, 15000, kRate, FilterType::LowPass, 500, 7);
  CHECK((spec.a == 5000u && spec.b == 15000u));
  const std::vector<std::int16_t> whole = run(spec, src);
  CHECK(same_outside(whole, src, 5000, 15000));
  CHECK(rms_of(whole, 10000, 15000) < 0.1 * rms_of(src, 10000, 15000));
  for (const std::uint32_t chunk : {1u, 7u, 333u, 4096u}) {
    std::vector<std::int16_t> out(n);
    EditRun st;
    for (std::uint32_t from = 0; from < n; from += chunk) {
      edit_chunk(spec, src.data(), n, out.data() + from, from, chunk, &st);
    }
    CHECK(out == whole);
  }
  // Silence in, silence out (no offset, no denormal noise); a resonant boost of a loud tone saturates instead of wrapping.
  const std::vector<std::int16_t> quiet(n, 0);
  for (const FilterType t : {FilterType::LowPass, FilterType::HighPass, FilterType::BandPass}) {
    CHECK(all_zero(run(make_filter_spec(n, 0, n, kRate, t, 1000, 100), quiet)));
  }
  const std::vector<std::int16_t> loud = sine_hz(n, 2000.0, 32000);
  const std::vector<std::int16_t> boosted = run(make_filter_spec(n, 0, n, kRate, FilterType::LowPass, 2000, 150), loud);
  std::int16_t top = 0;
  for (const std::int16_t v : boosted) {
    top = v > top ? v : top;
  }
  CHECK(top == 32767);
  // Out-of-range parameters are held: the coefficients stay finite.
  const EditSpec wild = make_filter_spec(n, 0, n, 0, FilterType::LowPass, 5000000, 0);
  for (const float k : wild.k) {
    CHECK(std::isfinite(k));
  }
  // Two channels are two runs: interleaving them gives each the result it has alone.
  const std::vector<std::int16_t> other = sine_hz(n, 300.0, 9000);
  const EditSpec full = make_filter_spec(n, 0, n, kRate, FilterType::HighPass, 1000, 7);
  const std::vector<std::int16_t> left = run(full, src);
  const std::vector<std::int16_t> right = run(full, other);
  std::vector<std::int16_t> l2(n);
  std::vector<std::int16_t> r2(n);
  EditRun sl;
  EditRun sr;
  for (std::uint32_t from = 0; from < n; from += 500) {
    edit_chunk(full, src.data(), n, l2.data() + from, from, 500, &sl);
    edit_chunk(full, other.data(), n, r2.data() + from, from, 500, &sr);
  }
  CHECK(l2 == left);
  CHECK(r2 == right);
}

TEST_CASE("sample dsp: the compressor takes the level above the threshold down by 1 - 1/ratio, leaves what is under it, and makes up (OS-AUDIT-15)") {
  const std::uint32_t n = 44100;
  // A held level of -6 dBFS (16384): threshold -18, ratio 4:1 -> 12 dB over, 9 dB of reduction -> -15 dBFS.
  const std::vector<std::int16_t> loud(n, 16384);
  const std::vector<std::int16_t> out = run(make_comp_spec(n, 0, n, kRate, -18, 40, 10, 100, 0, false, false), loud);
  CHECK(std::abs(db(static_cast<double>(out[n - 1]) / 16384.0) + 9.0) < 0.1);
  // Ratio 10:1 -> 10.8 dB of reduction.
  const std::vector<std::int16_t> o10 = run(make_comp_spec(n, 0, n, kRate, -18, 100, 10, 100, 0, false, false), loud);
  CHECK(std::abs(db(static_cast<double>(o10[n - 1]) / 16384.0) + 10.8) < 0.1);
  // Under the threshold nothing changes (makeup 0), with a makeup of 6 dB it is doubled.
  const std::vector<std::int16_t> soft(n, 1000);  // -29.7 dBFS
  CHECK(std::abs(run(make_comp_spec(n, 0, n, kRate, -18, 40, 10, 100, 0, false, false), soft)[n - 1] - 1000) <= 1);
  CHECK(std::abs(run(make_comp_spec(n, 0, n, kRate, -18, 40, 10, 100, 6, false, false), soft)[n - 1] - 1995) <= 2);
  // Auto makeup: (18 dB) * 3/4 = 13.5 dB on top.
  const std::vector<std::int16_t> au = run(make_comp_spec(n, 0, n, kRate, -18, 40, 10, 100, 0, true, false), loud);
  CHECK(std::abs(db(static_cast<double>(au[n - 1]) / 16384.0) - 4.5) < 0.1);
  // The attack: the first frames are not yet reduced, a slower attack reduces later.
  const std::vector<std::int16_t> fast = run(make_comp_spec(n, 0, n, kRate, -18, 40, 10, 100, 0, false, false), loud);
  const std::vector<std::int16_t> slow = run(make_comp_spec(n, 0, n, kRate, -18, 40, 5000, 100, 0, false, false), loud);
  CHECK(fast[0] > 15000);
  CHECK(slow[441] > fast[441]);
  CHECK(slow[n - 1] < slow[441]);
  // The release: after a loud burst the gain comes back, slower for a longer release.
  std::vector<std::int16_t> burst(n, 100);
  for (std::uint32_t i = 0; i < 10000; ++i) {
    burst[i] = 30000;
  }
  const std::vector<std::int16_t> r_fast = run(make_comp_spec(n, 0, n, kRate, -18, 40, 10, 20, 0, false, false), burst);
  const std::vector<std::int16_t> r_slow = run(make_comp_spec(n, 0, n, kRate, -18, 40, 10, 1000, 0, false, false), burst);
  CHECK(r_fast[20000] > r_slow[20000]);
  CHECK(std::abs(r_fast[n - 1] - 100) <= 1);
}

TEST_CASE("sample dsp: the compressor's detectors, selection, silence, saturation and chunking (OS-AUDIT-15)") {
  const std::uint32_t n = 40000;
  const std::vector<std::int16_t> src = sine_hz(n, 440.0, 28000);
  // RMS reads a sine 3 dB under its peak, so it reduces less than the peak detector at the same threshold.
  const double rp = rms_of(run(make_comp_spec(n, 0, n, kRate, -12, 40, 10, 100, 0, false, false), src), 20000, n);
  const double rr = rms_of(run(make_comp_spec(n, 0, n, kRate, -12, 40, 10, 100, 0, false, true), src), 20000, n);
  CHECK(rp < rms_of(src, 20000, n));
  CHECK(rr > rp);
  // Only the selection changes.
  const EditSpec sel = make_comp_spec(n, 10000, 30000, kRate, -24, 80, 5, 50, 0, false, false);
  const std::vector<std::int16_t> whole = run(sel, src);
  CHECK(same_outside(whole, src, 10000, 30000));
  CHECK(rms_of(whole, 20000, 30000) < rms_of(src, 20000, 30000));
  for (const std::uint32_t chunk : {1u, 13u, 1000u}) {
    std::vector<std::int16_t> out(n);
    EditRun st;
    for (std::uint32_t from = 0; from < n; from += chunk) {
      edit_chunk(sel, src.data(), n, out.data() + from, from, chunk, &st);
    }
    CHECK(out == whole);
  }
  // Silence stays silent even with makeup and auto makeup; a makeup that overs saturates.
  const std::vector<std::int16_t> quiet(n, 0);
  for (const bool au : {false, true}) {
    CHECK(all_zero(run(make_comp_spec(n, 0, n, kRate, -60, 200, 1, 5, 24, au, au), quiet)));
  }
  const std::vector<std::int16_t> hot = run(make_comp_spec(n, 0, n, kRate, 0, 11, 10, 100, 24, false, false), src);
  std::int16_t top = 0;
  for (const std::int16_t v : hot) {
    top = v > top ? v : top;
  }
  CHECK(top == 32767);
  // Wild parameters are held: the coefficients are finite.
  const EditSpec wild = make_comp_spec(n, 0, n, 0, -500, 0, 0, 0, 900, false, false);
  for (const float k : wild.k) {
    CHECK(std::isfinite(k));
  }
}

TEST_CASE("sample edit job: a filter and a compressor through the job equal the one-shot in steps of any size, over the selection only (OS-AUDIT-15)") {
  const std::uint32_t n = 30000;
  const std::vector<std::int16_t> src = sine_hz(n, 3000.0, 12000);
  for (const bool comp : {false, true}) {
    const EditSpec spec = comp ? make_comp_spec(n, 8000, 22000, kRate, -20, 60, 5, 80, 0, true, false) : make_filter_spec(n, 8000, 22000, kRate, FilterType::LowPass, 800, 10);
    std::vector<std::int16_t> out(n);
    SampleEditJob job;
    NEED(job.begin(spec, src.data(), n, out.data(), comp ? "COMP" : "FILTER"));
    unsigned steps = 0;
    unsigned last = 0;
    while (job.step(20) == o3os::core::JobStatus::Running && steps < 1000000u) {
      CHECK(job.progress() >= last);
      last = job.progress();
      ++steps;
    }
    CHECK(job.progress() == o3os::core::kJobProgressMax);
    CHECK(steps > 10u);
    CHECK_FALSE(job.unchanged());
    CHECK(out == run(spec, src));
    CHECK(same_outside(out, src, 8000, 22000));
    CHECK(out != src);
  }
  // A job starts clean: a second run over the same spec gives the same result.
  const EditSpec spec = make_filter_spec(n, 0, n, kRate, FilterType::HighPass, 2000, 7);
  std::vector<std::int16_t> a(n);
  std::vector<std::int16_t> b(n);
  SampleEditJob job;
  for (std::vector<std::int16_t>* dst : {&a, &b}) {
    NEED(job.begin(spec, src.data(), n, dst->data(), "F"));
    while (job.step(1000) == o3os::core::JobStatus::Running) {
    }
  }
  CHECK(a == b);
}

namespace {

// A sine of whole periods (so the source itself wraps without a jump), starting at phase 1 rad.
std::vector<std::int16_t> loop_sine(std::uint32_t frames, std::uint32_t period, int amp) {
  std::vector<std::int16_t> v(frames);
  for (std::uint32_t i = 0; i < frames; ++i) {
    v[i] = static_cast<std::int16_t>(std::lround(amp * std::sin(6.283185307179586 * i / period + 1.0)));
  }
  return v;
}

std::vector<std::int16_t> run_job(const EditSpec& spec, const std::vector<std::int16_t>& src, std::uint32_t budget_us) {
  const std::uint32_t n = static_cast<std::uint32_t>(src.size());
  std::vector<std::int16_t> out(n);
  SampleEditJob job;
  if (!job.begin(spec, src.data(), n, out.data(), "WARM")) {
    return {};
  }
  unsigned steps = 0;
  while (job.step(budget_us) == o3os::core::JobStatus::Running && steps < 1000000u) {
    ++steps;
  }
  CHECK(job.progress() == o3os::core::kJobProgressMax);
  return out;
}

// The step across the wrap (last frame to the first) against the largest step inside the loop.
double seam_ratio(const std::vector<std::int16_t>& y) {
  double slope = 0.0;
  for (std::size_t i = 1; i < y.size(); ++i) {
    slope = std::fmax(slope, std::fabs(static_cast<double>(y[i]) - y[i - 1]));
  }
  return std::fabs(static_cast<double>(y[0]) - y.back()) / slope;
}

}  // namespace

TEST_CASE("sample dsp: the filter warms up over the loop's tail, so a looped sine has no seam at the wrap (OS-A9-SEAM)") {
  const std::uint32_t n = 2000;  // 20 periods of 100 frames: the loop is the whole sample
  const std::vector<std::int16_t> src = loop_sine(n, 100, 10000);
  EditSpec spec = make_filter_spec(n, 0, n, kRate, FilterType::LowPass, 900, 7);
  const double cold = seam_ratio(run_job(spec, src, 50));  // without the fix: the filter starts from rest
  set_loop_warmup(spec, n, 0, n, kRate);
  CHECK(spec.warm_n == n);  // the loop is shorter than 300 ms
  const std::vector<std::int16_t> warm = run_job(spec, src, 50);
  MESSAGE("seam / slope without the warm-up: " << cold << ", with: " << seam_ratio(warm));
  CHECK(cold > 2.0);  // measured 2.97 (a jump of three times the steepest step); with the warm-up 0.97
  CHECK(seam_ratio(warm) < 1.5);
  CHECK(run_job(spec, src, 1000) == warm);  // the same result however the steps are cut
}

TEST_CASE("sample dsp: the compressor warms up over the loop's tail, its first frames match its last (OS-A9-SEAM)") {
  const std::uint32_t n = 20000;  // 453 ms: longer than the 300 ms warm-up cap
  const std::vector<std::int16_t> src = loop_sine(n, 100, 30000);
  EditSpec spec = make_comp_spec(n, 0, n, kRate, -24, 100, 100, 100, 0, false, false);
  const std::vector<std::int16_t> cold = run_job(spec, src, 50);
  set_loop_warmup(spec, n, 0, n, kRate);
  CHECK(spec.warm_n == kRate / 10u * 3u);
  CHECK(spec.warm_from == n - spec.warm_n);
  const std::vector<std::int16_t> warm = run_job(spec, src, 50);
  const double cold_ratio = rms_of(cold, 0, 300) / rms_of(cold, n - 300, n);
  const double warm_ratio = rms_of(warm, 0, 300) / rms_of(warm, n - 300, n);
  MESSAGE("first / last RMS without the warm-up: " << cold_ratio << ", with: " << warm_ratio);
  CHECK(cold_ratio > 1.3);
  CHECK(std::fabs(warm_ratio - 1.0) < 0.1);
}

TEST_CASE("sample dsp: no warm-up unless a Filter or Compressor range starts at the loop start, the result is then bit-exact (OS-A9-SEAM)") {
  const std::uint32_t n = 3000;
  const std::vector<std::int16_t> src = loop_sine(n, 100, 10000);
  const EditSpec filt = make_filter_spec(n, 500, n, kRate, FilterType::LowPass, 900, 7);
  EditSpec spec = filt;
  set_loop_warmup(spec, n, 0, n, kRate);  // the range starts after the loop start
  CHECK(spec.warm_n == 0u);
  set_loop_warmup(spec, n, 800, 400, kRate);  // an empty loop
  CHECK(spec.warm_n == 0u);
  CHECK(run_job(spec, src, 50) == run(filt, src));
  EditSpec amp = make_spec(EditOp::Amplify, n, 0, n, 200);
  set_loop_warmup(amp, n, 0, n, kRate);
  CHECK(amp.warm_n == 0u);
  EditSpec at = make_filter_spec(n, 500, n, kRate, FilterType::LowPass, 900, 7);
  set_loop_warmup(at, n, 500, 3000, 0);  // rate 0: 44,100, the loop is under the cap
  CHECK(at.warm_n == 2500u);
  CHECK(at.warm_from == 500u);
  set_loop_warmup(at, n, 500, 90000, kRate);  // a loop end past the sample is held to it
  CHECK(at.warm_from + at.warm_n == n);
}

// ---- OS-STEREO step 2, slice (d): the job over both blocks of a stereo sample (decision S7) ------------------------------------------------------------

namespace {

// A stereo fixture: M and S as the loader makes them (S = L - M), from a left and a right that differ in level and in DC; both blocks inside int16 with room to spare.
struct StereoFixture {
  std::uint32_t n;
  std::vector<std::int16_t> m, s, l, r;
  explicit StereoFixture(std::uint32_t frames, int amp_l = 9000, int amp_r = 4000) : n(frames), m(frames), s(frames), l(frames), r(frames) {
    for (std::uint32_t i = 0; i < frames; ++i) {
      l[i] = static_cast<std::int16_t>(std::lround(amp_l * std::sin(0.05 * i + 0.3)) + 700);
      r[i] = static_cast<std::int16_t>(std::lround(amp_r * std::sin(0.031 * i + 1.1)) - 300);
      m[i] = static_cast<std::int16_t>((l[i] + r[i]) / 2);
      s[i] = static_cast<std::int16_t>(l[i] - m[i]);
    }
  }
};

struct StereoOut {
  std::vector<std::int16_t> m, s;
  unsigned steps = 0;
  bool unchanged = false;
  bool held = false;
  bool began = false;
  EditSpec spec;
};

StereoOut run_stereo_job(const EditSpec& in, const StereoFixture& f, std::uint32_t budget_us) {
  StereoOut o;
  const std::uint32_t total = edit_result_frames(in, f.n);
  o.m.assign(total, 0x5A5A);
  o.s.assign(total, 0x5A5A);
  SampleEditJob job;
  o.began = job.begin(in, f.m.data(), f.n, o.m.data(), "ST", f.s.data(), o.s.data());
  if (!o.began) {
    return o;
  }
  unsigned last = 0;
  while (job.step(budget_us) == o3os::core::JobStatus::Running && o.steps < 1000000u) {
    CHECK(job.progress() >= last);  // never goes back, also when the pass over S starts
    last = job.progress();
    ++o.steps;
  }
  CHECK(job.progress() == o3os::core::kJobProgressMax);
  o.unchanged = job.unchanged();
  o.held = job.held();
  o.spec = job.spec();
  return o;
}

}  // namespace

TEST_CASE("sample edit job, stereo: Crop, Reverse, the fades and Amplify do the same to M and to S, in steps of any size; L and R follow (S7)") {
  const StereoFixture f(6001);  // odd: S sits at even(frames) in the sample bank, the job sees two plain blocks
  for (const EditOp op : {EditOp::Crop, EditOp::Reverse, EditOp::FadeIn, EditOp::FadeOut, EditOp::Amplify}) {
    const EditSpec spec = make_spec(op, f.n, 1500, 4500, 160);
    for (const std::uint32_t budget : {1u, 37u, 100000u}) {
      const StereoOut o = run_stereo_job(spec, f, budget);
      NEED(o.began);
      CHECK(o.m == run(spec, f.m));
      CHECK(o.s == run(spec, f.s));
      CHECK_FALSE(o.unchanged);
    }
    // L = M + S and R = M - S of the result against the edit of the file's own channels: exact for the copies, within a few LSB (the roundings of two blocks) for the scalings
    const StereoOut o = run_stereo_job(spec, f, 100000);
    const std::vector<std::int16_t> el = run(spec, f.l);
    const std::vector<std::int16_t> er = run(spec, f.r);
    int worst = 0;
    for (std::size_t i = 0; i < o.m.size(); ++i) {
      const int dl = std::abs(o.m[i] + o.s[i] - el[i]);
      const int dr = std::abs(o.m[i] - o.s[i] - er[i]);
      worst = std::max(worst, std::max(dl, dr));
    }
    const bool exact = op == EditOp::Crop || op == EditOp::Reverse;
    CHECK(worst <= (exact ? 1 : 3));  // (R is stored 1 LSB off when L + R is odd)
  }
  // The sources are never written.
  const StereoFixture g(6001);
  CHECK(f.m == g.m);
  CHECK(f.s == g.s);
}

TEST_CASE("sample edit job, stereo: Normalise takes one factor from the larger of |L| and |R| and applies it to both blocks, the balance is kept (S7)") {
  const StereoFixture f(5000, 12000, 3000);
  const EditSpec spec = make_spec(EditOp::Normalise, f.n, 0, f.n, 100);
  std::uint32_t peak_lr = 0;
  for (std::uint32_t i = 0; i < f.n; ++i) {
    peak_lr = std::max<std::uint32_t>(peak_lr, static_cast<std::uint32_t>(std::max(std::abs(f.m[i] + f.s[i]), std::abs(f.m[i] - f.s[i]))));
  }
  CHECK(scan_peak_ms(f.m.data(), f.s.data(), f.n, 0, f.n) == peak_lr);
  CHECK(peak_lr > scan_peak(f.m.data(), f.n, 0, f.n));  // the peak is not M's: M alone would have been normalised too loud
  const StereoOut o = run_stereo_job(spec, f, 777);
  NEED(o.began);
  const std::uint32_t gain = normalise_gain_q16(peak_lr);
  CHECK(o.spec.gain_q16 == gain);
  EditSpec used = spec;
  used.gain_q16 = gain;
  CHECK(o.m == run(used, f.m));
  CHECK(o.s == run(used, f.s));
  // The loudest of L and R reaches full scale (to the roundings of the two blocks) and nothing is above it.
  int peak_out = 0;
  for (std::size_t i = 0; i < o.m.size(); ++i) {
    peak_out = std::max(peak_out, std::max(std::abs(o.m[i] + o.s[i]), std::abs(o.m[i] - o.s[i])));
  }
  CHECK(peak_out >= 32765);
  CHECK(peak_out <= 32768);
  // A stereo sample that is at full scale in L or R has nothing to do, even when M alone is quiet.
  StereoFixture hot(2000, 3000, 1000);
  hot.m[10] = 16000;
  hot.s[10] = 16767;  // L = 32767, R = -767
  const StereoOut h = run_stereo_job(make_spec(EditOp::Normalise, 2000, 0, 2000, 100), hot, 100000);
  NEED(h.began);
  CHECK(h.unchanged);
}

TEST_CASE("sample edit job, stereo: DC remove and Filter run per block, the offset and the state of each block its own (S7)") {
  const StereoFixture f(8000);
  // DC remove: M and S each lose their own mean; L and R lose theirs.
  const EditSpec dc = make_spec(EditOp::DcRemove, f.n, 0, f.n, 100);
  DcScan sm;
  DcScan ss;
  scan_dc(f.m.data(), f.n, 0, f.n, sm);
  scan_dc(f.s.data(), f.n, 0, f.n, ss);
  const std::int32_t dm = dc_offset(sm, f.n);
  const std::int32_t ds = dc_offset(ss, f.n);
  CHECK(dm != 0);
  CHECK(ds != 0);
  CHECK(dm != ds);
  const StereoOut o = run_stereo_job(dc, f, 500);
  NEED(o.began);
  CHECK_FALSE(o.unchanged);
  EditSpec em = dc;
  em.dc = dm;
  EditSpec es = dc;
  es.dc = ds;
  CHECK(o.m == run(em, f.m));
  CHECK(o.s == run(es, f.s));
  double mean_l = 0;
  double mean_r = 0;
  for (std::size_t i = 0; i < o.m.size(); ++i) {
    mean_l += o.m[i] + o.s[i];
    mean_r += o.m[i] - o.s[i];
  }
  CHECK(std::abs(mean_l / static_cast<double>(o.m.size())) < 2.0);
  CHECK(std::abs(mean_r / static_cast<double>(o.m.size())) < 2.0);
  // No offset in either block: unchanged. An offset in S alone is not "nothing to do".
  StereoFixture none(1000);
  for (std::uint32_t i = 0; i < 1000; ++i) {
    none.m[i] = static_cast<std::int16_t>(i % 2 == 0 ? 500 : -500);
    none.s[i] = 0;
  }
  CHECK(run_stereo_job(make_spec(EditOp::DcRemove, 1000, 0, 1000, 100), none, 100000).unchanged);
  for (std::uint32_t i = 0; i < 1000; ++i) {
    none.s[i] = static_cast<std::int16_t>(i % 2 == 0 ? 300 : -100);  // mean +100
  }
  CHECK_FALSE(run_stereo_job(make_spec(EditOp::DcRemove, 1000, 0, 1000, 100), none, 100000).unchanged);
  // Filter: each block through a filter that starts from rest (S is not a continuation of M).
  const EditSpec fl = make_filter_spec(f.n, 1000, 7000, kRate, FilterType::LowPass, 700, 9);
  for (const std::uint32_t budget : {3u, 400u}) {
    const StereoOut x = run_stereo_job(fl, f, budget);
    NEED(x.began);
    CHECK(x.m == run(fl, f.m));
    CHECK(x.s == run(fl, f.s));
  }
  // The same with the loop warm-up of OS-A9-SEAM (the range starts at the loop start): the state is warmed per block as well.
  EditSpec wl = make_filter_spec(f.n, 0, f.n, kRate, FilterType::HighPass, 900, 7);
  set_loop_warmup(wl, f.n, 0, f.n, kRate);
  CHECK(wl.warm_n > 0u);
  const StereoOut w = run_stereo_job(wl, f, 50);
  NEED(w.began);
  CHECK(w.m == run_job(wl, f.m, 50));
  CHECK(w.s == run_job(wl, f.s, 50));
}

TEST_CASE("sample edit job, stereo: Compressor and Amiga are refused, and so is half a pair of blocks (S7)") {
  const StereoFixture f(4000);
  CHECK_FALSE(run_stereo_job(make_comp_spec(f.n, 0, f.n, kRate, -20, 60, 5, 80, 0, true, false), f, 100).began);
  CHECK_FALSE(run_stereo_job(make_amiga_spec(f.n, 428, false, false, 44100), f, 100).began);
  std::vector<std::int16_t> out(f.n);
  std::vector<std::int16_t> side(f.n);
  SampleEditJob job;
  const EditSpec amp = make_spec(EditOp::Amplify, f.n, 0, f.n, 150);
  CHECK_FALSE(job.begin(amp, f.m.data(), f.n, out.data(), "X", f.s.data(), nullptr));
  CHECK_FALSE(job.begin(amp, f.m.data(), f.n, out.data(), "X", nullptr, side.data()));
  CHECK(job.begin(amp, f.m.data(), f.n, out.data(), "X", f.s.data(), side.data()));
  CHECK(job.stereo());
  // A mono begin after a stereo job is mono again (the bank reuses its one job).
  CHECK(job.begin(amp, f.m.data(), f.n, out.data(), "X"));
  CHECK_FALSE(job.stereo());
}
