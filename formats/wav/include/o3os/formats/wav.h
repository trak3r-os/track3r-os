// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/core/byte_io.h>

#include <cstddef>
#include <cstdint>

namespace o3os::formats::wav {

// The WAV sample importer (OS-002c, ADR 0007 "Samples stay as WAV files", ADR 0009 rev 1 resumable formats). A resumable
// reader over core::ByteSource, in the shape of formats/native's Reader: begin(), then step(byte_budget) until Done or
// Failed, then report() and info(). It does no file I/O, allocates nothing, never blocks (a source that would block just ends
// the step) and trusts no length in the file (PAR-011): every size is checked against the RIFF, the bytes the source
// really delivers and the buffer it was given before a byte is written.
//
// What it reads
//   container  RIFF / WAVE, little-endian. RIFX, RF64 and BW64 are refused as unsupported.
//   format     PCM 8-bit (unsigned), 16-bit, 24-bit; IEEE float 32-bit; also in WAVE_FORMAT_EXTENSIBLE (the PCM and the float
//              sub-format). 32-bit integer PCM, 64-bit float and every compressed tag are refused as unsupported.
//   channels   1 or 2, any sample rate (>= 1 Hz); the rate is kept in Info (nothing is resampled: the engine plays at the
//              native rate). A stereo file is downmixed to mono, (L + R) / 2, by default (maintainer decision 2026-10-11; the engine's voices
//              play mono until OS-005d): the mean is formed at 24-bit precision and quantised with the dither below, so a pair
//              whose mean is not a whole 16-bit step is dithered and L = R passes bit-exact. `keep_stereo` keeps it stereo:
//              interleaved int16, frames * 2 samples. Either way the output is Info::frames * Info::channels samples.
//   data       streamed into `dst`, converted to int16: 8-bit and 16-bit exactly; 24-bit and float with TPDF dither (see
//              below); float NaN reads as 0 and infinities and overs clip. The data chunk must fit `dst` (an import never writes past
//              the sample bank, PAR-011): if it does not, nothing is written and the import fails LimitExceeded.
//   smpl       the root note and the first loop (forward, ping-pong or backward) become Info::loop_*; further loops are
//              counted in Report::loops_dropped.
//   cue        the cue points' sample offsets become slice points (strictly ascending, unique, inside the sample, at most
//              kMaxSlices = the instrument's 48: the lowest ones are kept, the rest counted in Report::slices_dropped).
//              Chunks before or after the data chunk are both read (streams cannot seek: the checks wait for the end).
//   clm        (OS-GRAN) Serum's wavetable marker: the text "<!>2048 20000000 wavetable (...)", whose first number is the frame size in samples. It becomes
//              Info::wt_frame (only a number in 1..65535; anything else is dropped and reported in Report::clm_ignored); what a frame size may be is the
//              caller's rule (formats/wav_sample.h), not the importer's.
//   other      LIST, bext, id3 and every unknown chunk are skipped. A malformed smpl or cue chunk is dropped and reported
//              (smpl_ignored / cue_ignored), never fatal: the audio is still good. Trouble in a chunk after the data chunk
//              ends the import there, successfully, with Report::trailing_ignored.
//
// Dither. Going from 24 bits or float to 16 loses information, so those samples get triangular (TPDF, +-1 LSB of the 16-bit
// result) dither from a fixed-seed xorshift generator: the output is a pure function of the input, which the tests and the
// simulator rely on. A sample that has nothing to lose (a 24-bit value whose low byte is zero, a float that is exactly a
// 16-bit value) gets none, so a 16-bit file stored in 24 bits or in float comes through bit-exact. Info::dithered says
// whether any sample was dithered.
//
// On Failed (or after cancel()) the first Info::frames * channels samples of `dst` hold whatever was converted so far: the
// caller discards the buffer. Memory: the Importer object, about 1.1 KB on the MK66 (512 of scratch, 192 of cue table, the Info),
// wherever the caller puts it (a job, a static).
// Contexts: main loop only.

inline constexpr std::size_t kScratchBytes = 512;
inline constexpr std::size_t kDefaultStepBudget = 4096;  // source bytes consumed per step() unless the job says otherwise
inline constexpr unsigned kMaxSlices = 48;               // = model::kSlices (checked in app, which sees both)

enum class Progress : std::uint8_t { Running, Done, Failed };

enum class Error : std::uint8_t {
  None,
  Truncated,      // the source ended inside a chunk, or before the data chunk was complete
  BadMagic,       // not a RIFF / WAVE file
  Unsupported,    // a valid file of a kind this importer does not take (RIFX, ADPCM, 5.1, 32-bit integer...)
  LimitExceeded,  // the audio does not fit the destination buffer
  Malformed,      // inconsistent sizes, a missing or duplicate chunk, a value out of range
  SourceFailed,   // the byte source reported a failure or broke its contract
  Cancelled,      // cancel() was called
};

const char* error_name(Error e) noexcept;

enum class SampleFormat : std::uint8_t { Pcm, Float };
enum class LoopType : std::uint8_t { Forward = 0, PingPong = 1, Backward = 2 };

struct Info {
  std::uint32_t sample_rate = 0;
  std::uint32_t frames = 0;     // complete frames written (valid from Done; counts up while running)
  std::uint8_t channels = 0;    // of the output: 1 or 2 (interleaved); 1 for a stereo file that was downmixed
  std::uint8_t source_channels = 0;  // of the file
  bool downmixed = false;       // a stereo file was mixed to mono (keep_stereo was off)
  bool side = false;            // OS-STEREO (b): the side signal S was written too, at dst[side_offset ...) (a stereo file read with `side`); false for a mono file or with `side` off
  std::uint32_t side_offset = 0;  // the index in dst where S starts: the frame count rounded up to even, so that S is word aligned too (valid when `side`)
  std::uint8_t bits = 0;        // of the file: 8, 16, 24, or 32 for float
  SampleFormat format = SampleFormat::Pcm;
  bool dithered = false;        // TPDF dither was applied to at least one sample
  std::uint8_t root_note = 60;  // MIDI note of the sample's own pitch (smpl; 60 when the file does not say)
  bool has_loop = false;
  LoopType loop_type = LoopType::Forward;
  std::uint32_t loop_start = 0;  // frames; the loop plays [loop_start, loop_end)
  std::uint32_t loop_end = 0;    // exclusive, <= frames (the file's inclusive last frame plus one)
  std::uint16_t wt_frame = 0;   // OS-GRAN: the frame size a 'clm ' chunk gave (Serum wavetable), 0 when the file has none
  std::uint8_t slice_count = 0;
  std::uint32_t slices[kMaxSlices] = {};  // frame offsets, strictly ascending, 1 .. frames - 1 (0 is the start, not a cut)
};

struct Report {
  Error error = Error::None;
  const char* detail = "";       // static text: what was wrong, "" when nothing was
  std::uint32_t offset = 0;      // bytes consumed from the source when the error was found
  std::uint32_t bytes = 0;       // bytes consumed so far
  // What the file had that was not used: never silent.
  bool smpl_ignored = false;       // a smpl chunk that was malformed, or whose loop is not usable
  bool cue_ignored = false;        // a cue chunk that was malformed
  bool clm_ignored = false;        // a 'clm ' chunk that did not say a frame size (OS-GRAN), or a second one
  bool trailing_ignored = false;   // the file went wrong after the audio: the rest was not read
  bool trailing_audio_bytes = false;  // the data chunk ended with a partial frame, which was dropped
  std::uint32_t loops_dropped = 0;
  std::uint32_t slices_dropped = 0;
};

class Importer {
 public:
  Importer() = default;
  Importer(const Importer&) = delete;
  Importer& operator=(const Importer&) = delete;

  // Starts reading `source` into `dst`, which has room for `capacity_samples` int16 values (frames * output channels: a
  // stereo file takes half of that when it is downmixed). Both must stay valid until Done, Failed or cancel(). Any run in
  // progress is abandoned. `keep_stereo` keeps a stereo file stereo; the default mixes it to mono, or (OS-LOADCH) `channel` 1 / 2 takes the left / right channel alone (0: the mean). `clip` (OS-FU2, the Sample load audition): a data chunk that does not fit is
  // not an error: its first whole frames that do fit are read and the import ends there, Done, with Report::trailing_ignored (loops and cues after the audio are not read).
  // OS-STEREO (b), `side`: a STEREO file is read as its mid M, the same mono mix as ever (bit for bit, dither included), at dst[0 .. frames), AND its side S = L - M (L as 16 bits, S clamped to 16 bits) at
  // dst[even(frames) .. 2 * even(frames)). Room for both is checked when the data chunk begins and the import fails LimitExceeded otherwise: it never clips (`clip` and `channel` are ignored with it;
  // `keep_stereo` wins over it). A mono file reads as ever. Info::side says whether S was written.
  void begin(core::ByteSource& source, std::int16_t* dst, std::size_t capacity_samples, bool keep_stereo = false, bool clip = false, std::uint8_t channel = 0, bool side = false) noexcept;
  // Consumes at most about `byte_budget` source bytes (at least one read is always made). Running: call again, on a later
  // pass of the main loop.
  Progress step(std::size_t byte_budget = kDefaultStepBudget) noexcept;
  // Ends a run in progress as Failed / Cancelled; a finished run is left as it is.
  void cancel() noexcept;

  Progress progress() const noexcept { return progress_; }
  // 0..1000: the share of the audio converted (999 until the file is fully read, 1000 when Done).
  unsigned permille() const noexcept;
  const Info& info() const noexcept { return info_; }
  const Report& report() const noexcept { return report_; }
  // Frames the audio will have when the run is Done (the data chunk's, clipped to the buffer in clip mode): 0 until the data chunk's header is read. With info().frames
  // (counting up) it tells how far a streamed read has come (OS-BUG-PREVLAT).
  std::uint32_t planned_frames() const noexcept { return data_seen_ && out_channels_ != 0 ? data_samples_ / out_channels_ : 0u; }

 private:
  enum class State : std::uint8_t {
    Riff, ChunkHead, Fmt, SmplHead, SmplLoop, CueHead, CueEntry, Clm, Data, Skip, Pad, Done, Failed
  };
  enum class Pull : std::uint8_t { Complete, Partial, End, Error };

  Pull read_some(void* dst, std::size_t ask, std::size_t& got) noexcept;
  bool take(std::size_t want) noexcept;
  void fail(Error e, const char* detail) noexcept;
  void finish() noexcept;
  void end_of_source() noexcept;
  void end_chunk() noexcept;
  void after_chunk() noexcept;
  void next_chunk() noexcept;
  void enter_skip(std::uint32_t n) noexcept;
  void on_riff() noexcept;
  void on_chunk_head() noexcept;
  void on_fmt() noexcept;
  void on_smpl_head() noexcept;
  void on_smpl_loop() noexcept;
  void on_cue_head() noexcept;
  void on_cue_entry() noexcept;
  void on_clm() noexcept;
  bool step_data() noexcept;
  void add_cue(std::uint32_t offset) noexcept;
  void convert(const std::uint8_t* src, std::size_t samples) noexcept;
  void convert_downmix(const std::uint8_t* src, std::size_t frames) noexcept;
  bool data_complete() const noexcept { return data_seen_ && convert_left_ == 0; }

  core::ByteSource* source_ = nullptr;
  std::int16_t* dst_ = nullptr;
  std::size_t capacity_ = 0;
  State state_ = State::Failed;
  Progress progress_ = Progress::Failed;
  Report report_;
  Info info_;

  std::size_t budget_ = 0;     // source bytes this step may still consume
  std::size_t want_ = 0;       // bytes the current state wants in scratch_
  std::size_t have_ = 0;
  std::uint64_t pos_ = 0;      // bytes consumed from the source
  std::uint64_t riff_end_ = 0; // offset of the end of the RIFF, kUnknownEnd when the file does not say
  std::uint32_t chunk_id_ = 0;
  std::uint32_t chunk_size_ = 0;
  std::uint32_t chunk_left_ = 0;  // payload bytes of the current chunk not consumed yet
  std::uint32_t skip_left_ = 0;

  bool fmt_seen_ = false;
  bool data_seen_ = false;
  bool float_ = false;
  bool keep_stereo_ = false;
  bool clip_ = false;             // OS-FU2: a too long data chunk is cut to the buffer instead of refused
  bool clipped_ = false;          // ... and it was
  bool downmix_ = false;          // a stereo file being mixed to mono
  bool want_side_ = false;        // OS-STEREO (b): begin() asked for the side signal
  bool side_ = false;             // ... and this file is stereo and takes it: convert_downmix() writes S at dst_ + side_off_
  std::uint32_t side_off_ = 0;    // even(frames of the data chunk)
  std::uint8_t pick_ = 0;         // OS-LOADCH: ... or reduced to one channel (1 left, 2 right; 0 the mean)
  std::uint8_t out_channels_ = 0;
  std::uint8_t bps_ = 0;          // bytes per sample: 1, 2, 3 or 4
  std::uint8_t unit_ = 0;         // bytes per conversion unit: a sample, or a whole frame when downmixing
  std::uint32_t convert_left_ = 0;   // bytes of whole frames of the data chunk still to be read
  std::uint32_t data_samples_ = 0;   // samples the data chunk will deliver
  std::uint32_t out_ = 0;            // samples written
  std::uint8_t carry_ = 0;           // bytes of a sample split across two reads, kept at the start of scratch_
  std::uint32_t rng_ = 0;

  bool clm_seen_ = false;
  bool smpl_seen_ = false;
  bool loop_seen_ = false;
  std::uint32_t loop_type_raw_ = 0;
  std::uint32_t loop_start_raw_ = 0;
  std::uint32_t loop_end_raw_ = 0;
  std::uint32_t cue_left_ = 0;       // cue entries still to read
  std::uint8_t cue_count_ = 0;
  std::uint32_t cues_[kMaxSlices] = {};  // the lowest kMaxSlices distinct offsets seen, ascending

  std::uint8_t scratch_[kScratchBytes];
};

}  // namespace o3os::formats::wav
