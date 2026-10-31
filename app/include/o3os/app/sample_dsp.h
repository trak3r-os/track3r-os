// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The offline effects of the Sample edit screen (OS-011a; BASE-16 lists the functions: normalise, cut, reverse, amplify and fades among 13): pure int16
// transforms from a source sample to a separate destination, resumable in chunks (the job, sample_edit_job.h, runs them a slice of the main loop at a time), host-tested
// (tests/sample_dsp_test.cpp). Nothing here touches the source: an edit is always written into another buffer, which is what lets the voices keep reading the old one
// until the engine is told (sample_bank.h, THE HANDOVER).
//
//   Crop       keeps frames [a, b): the result is b - a frames long
//   Normalise  scales [a, b) so that its peak is full scale (32767), clipping nothing; the rest is copied
//   Reverse    plays [a, b) backwards
//   Fade in    [a, b) from silence up to unity, linear (the first frame is 0, the last is the source's)
//   Fade out   [a, b) from unity down to silence (the first is the source's, the last 0)
//   Amplify    [a, b) times `percent` / 100, saturating at int16
//   DC remove  [a, b) minus its mean (OS-SEDFX), the shift held so that nothing clips; the rest is copied
//   Filter     [a, b) through one biquad, LP / HP / BP (OS-AUDIT-15, new Community OS behaviour; RBJ cookbook), cutoff in Hz and Q; its state starts at zero at `a`
//   Compressor [a, b) through a feed-forward compressor (OS-AUDIT-15): peak or RMS detector, threshold, ratio, attack, release, makeup (auto or dB); the gain follows in the log2 domain
//   Amiga      the WHOLE sample resampled for the Amiga voice, 8,287 or 16,574 Hz, optionally 8-bit (amiga_convert.h): a different length; the job runs it (it needs the kernel table), edit_chunk does not
//   Everything outside [a, b) is copied as it is (Crop: dropped).

#include <cstdint>

namespace o3os::app {

enum class EditOp : std::uint8_t { Crop, Normalise, Reverse, FadeIn, FadeOut, Amplify, Amiga, DcRemove, Filter, Compressor };
enum class FilterType : std::uint8_t { LowPass, HighPass, BandPass };

struct EditSpec {
  EditOp op = EditOp::Amplify;
  std::uint32_t a = 0;           // the range [a, b) of the source, in frames (a < b <= frames)
  std::uint32_t b = 0;
  std::uint32_t gain_q16 = 65536;  // Amplify and Normalise: the factor, Q16 (set by make_spec, and by the job for Normalise once the peak is known)
  std::int32_t dc = 0;              // DcRemove: the offset subtracted (set by the job once the range is scanned)
  std::uint32_t fade_step_q24 = 0;  // fades: the gain's step a frame, Q24 (2^24 / (n - 1))
  std::uint64_t step_q32 = 0;       // Amiga: source frames per output frame, Q32.32 (amiga_step_q32)
  std::uint16_t period = 0;         // Amiga: the Paula period of the target (428: 8,287 Hz, 214: 16,574 Hz)
  bool q8 = false;                  // Amiga: round to 8-bit levels
  bool rms = false;                 // Compressor: the detector is RMS (10 ms), else peak
  // Filter: b0 b1 b2 a1 a2 (divided by a0), set by make_filter_spec. Compressor: threshold and makeup in log2 units, 1 - 1 / ratio, the attack and release smoothing coefficients, the RMS coefficient
  // (make_comp_spec). Floats computed once for the job; the per-frame loop is a few multiplies (offline, never in the audio path).
  float k[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  // Filter and Compressor on a looped sample (OS-A9-SEAM): frames [warm_from, warm_from + warm_n) of the source run through the state first, the output discarded, so the range starts as
  // if it came out of the loop's end (set_loop_warmup). 0: none, the state starts from rest.
  std::uint32_t warm_from = 0;
  std::uint32_t warm_n = 0;
};

// The running state of Filter and Compressor, carried from one chunk of edit_chunk to the next (the job owns one, reset at the start). Chunks must come in order.
struct EditRun {
  float z1 = 0.0f;  // Filter: the biquad's two states (transposed direct form II)
  float z2 = 0.0f;
  float gr = 0.0f;  // Compressor: the smoothed gain reduction (log2 units), and the RMS detector's mean square
  float ms = 0.0f;
};

// The spec of `op` over [a, b) of a sample of `frames` (a and b are put in order and held inside it; an empty range is the whole sample). `percent` is Amplify's
// (10..800 are the screen's range; any value is taken, the gain is held to 255x).
EditSpec make_spec(EditOp op, std::uint32_t frames, std::uint32_t a, std::uint32_t b, unsigned percent) noexcept;

// The spec of a FILTER over [a, b) (an empty range is the whole sample) of a sample of `frames` at `rate_hz` (0: 44,100): `cutoff_hz` is held to 20 Hz .. 0.45 rate, `q_x10` (Q times ten) to 0.5 .. 15.
EditSpec make_filter_spec(std::uint32_t frames, std::uint32_t a, std::uint32_t b, unsigned rate_hz, FilterType type, unsigned cutoff_hz, unsigned q_x10) noexcept;
// The spec of a COMPRESSOR: threshold -60..0 dB, ratio x10 11..200, attack ms x10 1..1000, release 5..2000 ms, makeup 0..24 dB or `auto_makeup` (the reduction a full-scale peak would get, held to
// 24 dB). Values outside are held to the range.
EditSpec make_comp_spec(std::uint32_t frames, std::uint32_t a, std::uint32_t b, unsigned rate_hz, int threshold_db, unsigned ratio_x10, unsigned attack_ms_x10, unsigned release_ms, int makeup_db,
                        bool auto_makeup, bool rms) noexcept;
// OS-A9-SEAM: Filter and Compressor on a range that starts at the loop start of the instrument in view (`loop_start`, `loop_end` in frames, a forward or backward loop) warm their state over the
// loop's tail first, at most 300 ms of the sample's `rate_hz` (0: 44,100) or the loop, so the wrap has no seam. Any other op, range or loop leaves `spec` as it is (bit-exact).
void set_loop_warmup(EditSpec& spec, std::uint32_t frames, std::uint32_t loop_start, std::uint32_t loop_end, unsigned rate_hz) noexcept;
// How many times dearer than a gain a frame of `op` is: the job divides its frame budget by it (its kFramesPerUs is for the cheap ones).
unsigned edit_cost(EditOp op) noexcept;

// The spec of the Amiga conversion of a sample of `frames` (the whole of it): `period` kAmigaPeriod8 or kAmigaPeriod16, `src_amiga` when the instrument plays on the Amiga voice now.
EditSpec make_amiga_spec(std::uint32_t frames, unsigned period, bool src_amiga, bool q8, std::uint32_t src_rate_hz = 0) noexcept;

// Frames the result has: b - a for Crop, the new length for Amiga, `frames` for the others.
std::uint32_t edit_result_frames(const EditSpec& spec, std::uint32_t frames) noexcept;

// The largest |sample| of frames [from, from + n) (clamped to the source): 0 for silence.
std::uint32_t scan_peak(const std::int16_t* src, std::uint32_t frames, std::uint32_t from, std::uint32_t n) noexcept;
// OS-STEREO (d): the largest |M| + |S| of frames [from, from + n) of a stereo sample (M at `mid`, S at `side`, S = L - M, so L = M + S and R = M - S: |M| + |S| is the larger of |L| and |R|, to
// within the 1 LSB of the stored R). Normalise's peak for a stereo sample: one factor for both blocks keeps the balance.
std::uint32_t scan_peak_ms(const std::int16_t* mid, const std::int16_t* side, std::uint32_t frames, std::uint32_t from, std::uint32_t n) noexcept;
// DC removal's scan of a range: the sum (int64: frames of 16 bits, a 16 M-frame range needs 2^39, 2^47 holds 2^32 of them) and the extremes.
struct DcScan {
  std::int64_t sum = 0;
  std::int32_t lo = 32767;
  std::int32_t hi = -32768;
};
// Adds frames [from, from + n) (clamped to the source) to `scan`.
void scan_dc(const std::int16_t* src, std::uint32_t frames, std::uint32_t from, std::uint32_t n, DcScan& scan) noexcept;
// The offset to subtract from a range of `n` frames that was scanned: its mean rounded to nearest, held so that no frame leaves int16 (the range is then partly centred only
// when it already touches full scale on the side the mean would push towards). 0 for no frames. `held` (when given) is set when the offset was held back from the mean.
std::int32_t dc_offset(const DcScan& scan, std::uint32_t n, bool* held = nullptr) noexcept;
// Normalise's factor for a peak: 32767 / peak in Q16, never below unity (a sample already at full scale stays); 0 peak is unity.
std::uint32_t normalise_gain_q16(std::uint32_t peak) noexcept;

// Writes result frames [from, from + n) of the edit into dst[0..n): dst is the chunk, not the whole result. `from + n` is held to the result's length.
// `run` carries Filter's and Compressor's state across chunks (null: a fresh one, right only for a whole result in one call); the other effects ignore it.
void edit_chunk(const EditSpec& spec, const std::int16_t* src, std::uint32_t frames, std::int16_t* dst, std::uint32_t from, std::uint32_t n, EditRun* run = nullptr) noexcept;
// Warm-up frames [from, from + n) of spec.warm_n (OS-A9-SEAM): runs source frames warm_from + from .. through `run`, writes nothing. Chunks in order, all before the first edit_chunk.
void edit_warm(const EditSpec& spec, const std::int16_t* src, std::uint32_t frames, std::uint32_t from, std::uint32_t n, EditRun* run) noexcept;

}  // namespace o3os::app
