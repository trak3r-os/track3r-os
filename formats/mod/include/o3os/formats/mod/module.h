// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/formats/med/mmd.h>

#include <cstddef>
#include <cstdint>

namespace o3os::formats::mod {

// ProTracker MOD and Scream Tracker 3 S3M module files (OS-MODIMP, new Community OS behaviour; docs/formats/mod.md): the reader. Like the OctaMED reader (med/mmd.h) it works on a
// random-access source and never trusts a number: every offset is range-checked in 64-bit arithmetic, every count is capped by `caps`, nothing recurses, nothing allocates, integers are
// decoded by hand (never by struct punning), and a file of any content ends in a typed Status. Nothing here knows the project model: the mapping is import.h.
//
// MOD: M.K., M!K!, M&K!, N.T. (4 channels), FLT4, FLT8 (8 channels, patterns stored in pairs), 1CHN..9CHN and 10CH..32CH. The 15-sample Soundtracker files have no signature: open() takes one when its structure holds
// (is_soundtracker15, OS-IMPORT-ANY I6), sniff_kind() never claims it. S3M: the "SCRM" header, PCM instruments (8 or 16 bit, signed or unsigned, a stereo sample reads its left channel), packed patterns; AdLib instruments and ADPCM-packed samples are
// reported and refused. Contexts: main loop only (a job); a Module is about 11 KB (the decoded S3M pattern in view is in it), the rest is read on demand.

namespace caps {
inline constexpr std::uint32_t kMaxChannels = 32;
inline constexpr std::uint32_t kRows = 64;                // every MOD and S3M pattern has 64 rows
inline constexpr std::uint32_t kMaxModOrders = 128;
inline constexpr std::uint32_t kMaxS3mOrders = 255;       // ST3's own limit: the order list is at most 255 bytes
inline constexpr std::uint32_t kMaxModPatternNumber = 127;
inline constexpr std::uint32_t kMaxS3mPatterns = 255;
inline constexpr std::uint32_t kMaxS3mInstruments = 255;  // the header's count; only the first kSlots are imported (ST3 has 99)
inline constexpr std::uint32_t kModSamples = 31;
inline constexpr std::uint32_t kSlots = 63;               // instrument numbers imported: the MED importer's (model slot = number - 1)
inline constexpr std::uint32_t kSampleNameMax = 32;       // the instrument name of the model
inline constexpr std::uint32_t kS3mHeaderBytes = 96;
inline constexpr std::uint32_t kModHeaderBytes = 1084;    // 31 samples
inline constexpr std::uint32_t kPackedMax = 65535;        // a packed S3M pattern: the u16 length in front of it
// The work an import may make the S3M pattern decoder do: packed bytes decoded, all phases together. A real song decodes each pattern a few times (about 3 KB each); this allows
// 32 MiB, which a hostile file reaches only by switching between large patterns on every row (the flow caps the visits, not the bytes).
inline constexpr std::uint64_t kDecodeBudget = 32ull * 1024ull * 1024ull;
}  // namespace caps

enum class Kind : std::uint8_t { None, Mod, S3m };

using med::Error;
using med::RandomSource;
using med::SpanSource;
using med::Status;

// One cell of a row, as the file says it (the mapping to our model is import.h).
//   MOD  note: 1..36 = the nearest entry of ProTracker's period table (856 = C-1 = 1 ... 113 = B-3 = 36), 0 none; period: the raw 12 bits; effect 0..15 (the E commands keep their
//        sub-command in param's high nibble), volume 0xFF.
//   S3M  note: octave x 12 + semitone + 1 (1..192), 0 none; cut: the "^^" note (254); volume: the volume column 0..64, 0xFF none; effect 1..26 = A..Z, 0 none.
struct Cell {
  std::uint8_t note = 0;
  bool cut = false;
  std::uint8_t instrument = 0;     // 0 none (MOD 1..31, S3M 1..255: the importer takes the first 63)
  std::uint8_t volume = 0xFF;
  std::uint8_t effect = 0;
  std::uint8_t param = 0;
  std::uint16_t period = 0;        // MOD: the raw period
  bool off_table = false;          // MOD: the period is not in the table (the note is the nearest, clamped to the table's ends)
  bool bad_note = false;           // S3M: a note byte whose semitone is above 11 (read as the next octave's, as libopenmpt does)
};

enum class SampleKind : std::uint8_t {
  Empty,        // no data (a MOD sample of length 0, an S3M instrument of type 0, or a PCM one of no frames)
  Pcm,
  AdLib,        // S3M instrument types 2..7: refused
  Unsupported,  // S3M packed (ADPCM) sample data: refused
};

enum SampleNote : std::uint8_t {
  kNoteTrimmed = 1u << 0,       // the data runs past the end of the file: the frames that are there
  kNoteLoopClamped = 1u << 1,   // the loop was cut to lie inside the sample
  kNoteStereo = 1u << 2,        // an S3M stereo sample: its left channel
  kNoteOversize = 1u << 3,      // the PCM alone would not fit the sample arena
};

struct SampleInfo {
  SampleKind kind = SampleKind::Empty;
  std::uint8_t notes = 0;         // SampleNote bits
  std::uint32_t offset = 0;       // the first byte of the data
  std::uint32_t frames = 0;       // of the data that is in the file
  bool sixteen_bit = false;
  bool is_signed = true;          // MOD always; S3M by the header's sample format (ffi)
  std::uint8_t volume = 0;        // 0..64
  std::int8_t finetune = 0;       // MOD -8..7 (the low nibble of the header byte, 8..15 = -8..-1); 0 for S3M
  std::uint32_t c2spd = 0;        // S3M: Hz of the middle C (0 in the file: 8363); MOD 0 (the Paula rate)
  bool has_loop = false;
  std::uint32_t loop_start = 0;   // frames
  std::uint32_t loop_len = 0;
  std::uint8_t s3m_type = 0;      // S3M instrument type byte
  char name[caps::kSampleNameMax + 1] = {};  // sanitised: bytes outside 0x20..0x7E are '?'
};

enum class Variant : std::uint8_t { None, Mk, Mk2, Nt, Flt4, Flt8, Chn, S3m, St15 };  // St15: a 15-sample Soundtracker file (no tag; read as 4 channels)

struct ModuleInfo {
  Kind kind = Kind::None;
  Variant variant = Variant::None;
  char tag[5] = {};                 // the signature as the file has it, "SCRM" for an S3M
  std::uint32_t file_size = 0;
  std::uint8_t channels = 0;        // MOD: the tag's; S3M: the highest channel with a setting other than 255, plus one
  std::uint16_t order_count = 0;    // entries of the order list as it plays (S3M: up to the first 255 marker)
  std::uint16_t pattern_count = 0;  // patterns in the file (MOD FLT8: of 8 channels)
  std::uint16_t sample_count = 0;   // MOD 31; S3M the header's instrument count
  std::uint8_t restart = 0;         // MOD: the byte after the song length (127 = none)
  std::uint8_t speed = 6;           // ticks a row at the start
  std::uint8_t tempo = 125;         // BPM at the start
  char name[29] = {};               // sanitised
  // S3M
  std::uint8_t global_volume = 64;
  std::uint8_t master_volume = 0;   // the header byte: bit 7 stereo, the rest the volume
  bool stereo = false;
  bool signed_samples = false;      // ffi 1
  bool fast_slides = false;         // flags bit 6: ST3.00 volume slides also run on tick 0
  bool amiga_limits = false;
  std::uint16_t cwtv = 0;
  std::uint8_t skipped_orders = 0;  // order entries 254 (skipped) and entries behind the end marker
  std::uint8_t bad_orders = 0;      // order entries that name a pattern the file does not have
  std::uint8_t adlib_channels = 0;  // channels whose setting says AdLib
  std::uint8_t muted_channels = 0;
  std::uint8_t chan_pan[caps::kMaxChannels] = {};   // percent 0..100 (the file's default panning)
  bool chan_used[caps::kMaxChannels] = {};
  bool chan_muted[caps::kMaxChannels] = {};
  bool chan_adlib[caps::kMaxChannels] = {};
};

class Module {
 public:
  // Reads the header, the order list and (S3M) the parapointer tables of `source`, which must outlive the Module. Everything else is read on demand. A failed open leaves the Module
  // blank. A file that is neither an S3M nor a MOD with a known tag is BadMagic.
  Status open(RandomSource& source) noexcept;

  const ModuleInfo& info() const noexcept { return info_; }
  RandomSource& source() const noexcept { return *src_; }

  // The pattern number at order position `i` < info().order_count (an entry the file does not have a pattern for, or a 254 marker, is kSkip). MOD FLT8: the 8-channel pattern.
  static constexpr std::uint8_t kSkip = 0xFF;
  std::uint8_t order(std::uint32_t i) const noexcept { return i < info_.order_count ? order_[i] : kSkip; }

  // Row `row` (< 64) of pattern `pattern` (< info().pattern_count) into out[0 .. info().channels). Not const: the S3M decoder keeps the pattern in view. A pattern that is not in the file
  // (an S3M parapointer of 0) is 64 empty rows. LimitExceeded when the decoder has made more work than caps::kDecodeBudget.
  Status read_line(std::uint32_t pattern, std::uint32_t row, Cell* out) noexcept;

  // Instrument / sample `slot` < info().sample_count (MOD: slot + 1 is the sample number; S3M: the instrument number).
  Status sample(std::uint32_t slot, SampleInfo& out) const noexcept;
  // `n` mono frames from frame `first` of the sample (as sample() says it) into dst as 16-bit: 8-bit data times 256, unsigned data re-centred, 16-bit as it is.
  Status read_pcm(const SampleInfo& s, std::uint32_t first, std::uint32_t n, std::int16_t* dst) const noexcept;

 private:
  Status get(std::uint32_t offset, void* dst, std::uint32_t n) const noexcept;
  Status open_mod(const std::uint8_t* head1084) noexcept;
  Status open_s3m(const std::uint8_t* head96) noexcept;
  Status decode_s3m(std::uint32_t pattern) noexcept;
  Status mod_line(std::uint32_t pattern, std::uint32_t row, Cell* out) const noexcept;

  RandomSource* src_ = nullptr;
  ModuleInfo info_;
  std::uint8_t order_[caps::kMaxS3mOrders] = {};
  // MOD
  std::uint32_t pat_offset_ = 0;            // the first pattern
  std::uint32_t sample_offset_ = 0;         // the first byte of sample data
  std::uint32_t smp_off_[caps::kModSamples] = {};  // where each sample's data begins
  // S3M
  std::uint16_t ins_ptr_[caps::kMaxS3mInstruments] = {};   // the parapointers of the instruments: bytes / 16
  std::uint16_t pat_ptr_[caps::kMaxS3mPatterns] = {};
  std::uint32_t cached_ = 0xFFFFFFFFu;       // the pattern in `cache_`
  std::uint64_t decoded_ = 0;                // packed bytes decoded so far
  // 64 rows x 32 channels x (note, instrument, volume, effect, param)
  std::uint8_t cache_[caps::kRows * caps::kMaxChannels * 5] = {};
};

// What the first `n` bytes of a file say it is: an S3M ("SCRM" at 44, 0x1A and type 16 before it: needs 48 bytes) or a MOD whose tag is one this reader takes (at 1,080: needs 1,084 bytes).
// Kind::None when they say neither, and for a head too short to tell.
Kind sniff_kind(const std::uint8_t* head, std::size_t n) noexcept;

// OS-IMPORT-ANY I6: does the structure of a file say it is a 15-sample Soundtracker module? Such a file has no signature, so this never decides a listing or a sniff (sniff_kind says None): a caller asks it
// when the name says MOD or says nothing. `head` holds the first `n` bytes (the 600-byte header at least) and `file_size` is the file's. True when: the song length is 1..128; every order the song plays is below 64;
// every sample volume is at most 64; the title and the sample names are printable ASCII or NUL; and the file holds the header, the patterns (the highest order of the table + 1, or of the played ones when the whole
// table does not fit) of 1,024 bytes each, and the samples, the last one allowed to be short. open() reads such a file as a 4-channel M.K. module with 15 samples (Variant::St15).
bool is_soundtracker15(const std::uint8_t* head, std::size_t n, std::uint32_t file_size) noexcept;

// The Paula period of ProTracker's table at finetune 0 for the 36 notes C-1 .. B-3: index 0..35.
extern const std::uint16_t kPeriodTable[36];

}  // namespace o3os::formats::mod
