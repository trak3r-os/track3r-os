// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/core/byte_io.h>
#include <o3os/formats/crc32.h>
#include <o3os/formats/progress.h>
#include <o3os/model/project.h>

#include <cstddef>
#include <cstdint>

namespace o3os::formats::native {

// The native project format (ADR 0007): an IFF-style container, specified byte by byte in
// docs/formats/o3-project.md, with a stdlib-Python reference in tools/o3project.py. This header is the
// C++ side: resumable reader and writer over core::ByteSource / ByteSink. Neither does file I/O, allocates,
// blocks or trusts a length: they run a bounded amount of work per step() so a cooperative job (OS-002c)
// can drive them, and any input, however hostile, ends in Done or in a typed error with the model left as a
// valid empty project.

constexpr std::uint32_t fourcc(char a, char b, char c, char d) noexcept {
  return (static_cast<std::uint32_t>(static_cast<unsigned char>(a)) << 24) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 16) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 8) |
         static_cast<std::uint32_t>(static_cast<unsigned char>(d));
}

inline constexpr std::uint32_t kForm = fourcc('F', 'O', 'R', 'M');
inline constexpr std::uint32_t kTypeO3pj = fourcc('O', '3', 'P', 'J');
inline constexpr std::uint32_t kChunkPhdr = fourcc('P', 'H', 'D', 'R');  // project header
inline constexpr std::uint32_t kChunkTrak = fourcc('T', 'R', 'A', 'K');  // track table
inline constexpr std::uint32_t kChunkModr = fourcc('M', 'O', 'D', 'R');  // module references
inline constexpr std::uint32_t kChunkSong = fourcc('S', 'O', 'N', 'G');  // order list
inline constexpr std::uint32_t kChunkPatt = fourcc('P', 'A', 'T', 'T');  // one pattern
inline constexpr std::uint32_t kChunkInst = fourcc('I', 'N', 'S', 'T');  // one instrument
inline constexpr std::uint32_t kChunkSets = fourcc('S', 'E', 'T', 'S');  // the project's settings (OS-002d, docs/formats/o3-settings.md)
inline constexpr std::uint32_t kChunkLrnb = fourcc('L', 'R', 'N', 'B');  // the project's MIDI Learn bindings (OS-002d)
inline constexpr std::uint32_t kChunkMidi = fourcc('M', 'I', 'D', 'I');  // the project's MIDI routing, offsets and CC numbers (OS-004d)
inline constexpr std::uint32_t kChunkUsbs = fourcc('U', 'S', 'B', 'S');  // the USB mode, profile and stem map (OS-004d)
inline constexpr std::uint32_t kChunkMixr = fourcc('M', 'I', 'X', 'R');  // the mixer's routing matrix, OUT mode and levels (OS-004d)
inline constexpr std::uint32_t kChunkPerf = fourcc('P', 'E', 'R', 'F');  // the twelve Perform slots (OS-004d)
inline constexpr std::uint32_t kChunkAppm = fourcc('A', 'P', 'P', 'M');  // the parameters of SD apps, by module name (OS-004d)
inline constexpr std::uint32_t kChunkNote = fourcc('N', 'O', 'T', 'E');  // the note-entry settings and JUMP (OS-PERSIST2)
inline constexpr std::uint32_t kChunkMute = fourcc('M', 'U', 'T', 'E');  // each track's MUTE MODE (OS-PERSIST2)
inline constexpr std::uint32_t kChunkScen = fourcc('S', 'C', 'E', 'N');  // the Perform scenes and LATCH (OS-PERSIST2)
inline constexpr std::uint32_t kChunkSnfx = fourcc('S', 'N', 'F', 'X');  // the send effects' controls (OS-PERSIST2)
inline constexpr std::uint32_t kChunkLaw = fourcc('L', 'A', 'W', ' ');   // the instrument volume law, written only when it is the 1.1.14 table (OS-BUG-PASTE24)
inline constexpr std::uint32_t kChunkCrc = fourcc('C', 'R', 'C', ' ');   // CRC-32 of everything before it

// Chunk version, the first two payload bytes of every chunk: major in the high byte, minor in the low byte.
// A reader accepts the same major with any minor (it skips fields it does not know and says so in the
// report), refuses a newer major.
inline constexpr std::uint16_t kChunkVersion = 0x0100;
// INST is at 1.1 (OS-002c): version 1.0 plus one appended field, the sample's rate (u32, 0 = not recorded). The first
// chunk to use the minor version: a reader that only knows 1.0 skips the four bytes and reports newer_minor, a reader
// that knows 1.1 reads a 1.0 chunk as rate 0.
inline constexpr std::uint16_t kInstVersion = 0x0101;
// OS-004d: INST 1.2 appends one more field after the rate, the instrument's playback engine (u8, model::kInstEngine*, 0 = automatic). The writer writes 1.2 only for an instrument whose
// engine is not automatic and 1.1 for every other, so a project that never chose an engine is the file it always was (and an older reader meets no newer minor in it); a reader that knows
// 1.2 reads a 1.1 chunk as engine 0, and a value above model::kInstEngineMax as 0 (a later engine this version does not have plays as automatic).
inline constexpr std::uint16_t kInstVersionEngine = 0x0102;
// OS-INSTUI S8b + S9: INST 1.3 appends the twelve bytes of a MIDI instrument's fields and a sample instrument's VEL> after the engine byte (docs/formats/INST-1.3-diff.md; model::kMidi*). The engine byte is then always
// present (0 for automatic). The writer writes 1.3 only for an instrument one of whose twelve bytes differs from its default (model::inst_v13_default), else 1.2 or 1.1 as before, so a project that never used them is the
// file it always was (and an older reader meets no newer minor in it). A reader that knows 1.3 reads 1.2 and 1.1 as the defaults and a value out of a field's range as that field's default; an older reader meets a newer
// minor of INST, reads the instrument as of 1.2 and loses the twelve bytes on its next save.
inline constexpr std::uint16_t kInstVersionMidi = 0x0103;
// OS-LFOSINE: INST 1.4 adds no field and no byte (the layout is 1.3's, the engine byte and the twelve MIDI bytes always present): the version only says that an LFO shape of 5 is Sine (docs/specs/OS-LFOSINE.md).
// The writer writes 1.4 only for an instrument one of whose six LFOs is Sine (model::inst_uses_sine), else the older minor as before. A reader that knows 1.4 reads a shape of 5 in an older chunk as Triangle (which it played)
// and a shape above 5 in any chunk as Triangle; an older reader meets a newer minor with nothing appended, reads the chunk whole and plays the shape as Triangle.
inline constexpr std::uint16_t kInstVersionLfoSine = 0x0104;
// OS-STEREO step 2 (NEW COMMUNITY OS BEHAVIOUR): INST 1.5 appends one byte after the twelve MIDI bytes, the instrument's stereo flag (u8, 0 mono, 1 stereo: model::Instrument::stereo). The layout before it is 1.3's, whole (the engine byte
// and the twelve MIDI bytes always present), and the Sine LFO gate stays "minor >= 4" (1.5 is above it). The writer writes 1.5 only for a Sample instrument with the flag set, so every project without a stereo instrument is the file it
// was (byte for byte). A reader that knows 1.5 reads older chunks as 0 and a value above 1 as 0; a chunk that sets the flag on a Module or MIDI instrument is Malformed (model::validate_instrument). An older reader meets a newer minor, reads
// the chunk as of 1.4 and loses the byte on its next save (the instrument then plays mono, by design).
inline constexpr std::uint16_t kInstVersionStereo = 0x0105;
// OS-PERFFX P1 (decision D5, option A): PERF and SCEN 1.1 add no field and no byte: the version only says that the Perform FX ids 20 (BITDEPTH) and 21 (OVERDRV) are valid. The writer writes 1.1 only when a slot
// (of a stored scene, for SCEN) holds one of them and 1.0, byte for byte as before, for every other bank (state_chunks::version_of); a reader that knows 1.1 reads 1.0 with 20 and 21 as none. An older reader
// meets 1.1 as a newer minor of a chunk it knows at 1.0: it reads the chunk whole (the length is the same), reads the two ids as none and loses them on its next save.
inline constexpr std::uint16_t kStateVersionFx = 0x0101;

// Working memory of Reader and Writer (one piece of chunk at a time), and the work done per step() by default.
inline constexpr std::size_t kScratchBytes = 1024;
inline constexpr std::size_t kDefaultStepBudget = 4096;

// Largest accepted file. The known chunks of a full project add up to about 1.9 MB; the rest of the room is
// for unknown chunks, which are skipped (or kept, up to the model's pool).
inline constexpr std::uint32_t kMaxFileBytes = 4u << 20;

using formats::Progress;  // the one progress type of every format (formats/progress.h)

enum class Error : std::uint8_t {
  None,
  Truncated,      // the source ended before the FORM did
  BadMagic,       // not an O3PJ FORM
  BadCrc,         // the CRC chunk does not match what was read
  NewerMajor,     // a chunk written by a newer major version of the format
  LimitExceeded,  // more patterns, instruments or modules than the model holds, or a file over kMaxFileBytes
  Malformed,      // inconsistent lengths, a value out of range, a chunk out of order, a duplicate...
  SourceFailed,   // the byte source reported a failure or broke its contract
  SinkFailed,     // the byte sink reported a failure or broke its contract
  Modified,       // writer only: the project changed while it was being written
  Internal,       // writer only: its size formulas disagree with what it produced (a bug, never input)
};

const char* error_name(Error e) noexcept;

struct Report {
  Error error = Error::None;
  const char* detail = "";    // static text: what was wrong, "" when nothing was
  std::uint32_t offset = 0;   // bytes consumed (reader) or produced (writer) when the error was found
  std::uint32_t chunk = 0;    // fourcc of the chunk being processed (0 before the first)
  std::uint32_t bytes = 0;    // bytes consumed / produced so far
  std::uint32_t patterns = 0;     // PATT chunks applied (reader)
  std::uint32_t instruments = 0;  // INST chunks applied (reader)
  // Unknown chunks (reader, on Done): kept in the model's pool and re-emitted on save, and dropped because the
  // pool was full (the UI tells the user; the same counts stay readable on the Project).
  std::uint32_t unknown_kept = 0;
  std::uint32_t unknown_dropped = 0;
  // Entries of the SETS and LRNB chunks that were not taken (a setting this version does not have, a value out of its range, an
  // invalid binding) and values of the session state chunks NOTE, MUTE, SCEN and SNFX that were out of range (OS-PERSIST2): the setting
  // keeps its default, the load goes on (reader, OS-002d).
  std::uint32_t settings_skipped = 0;
  // A chunk had a newer minor version than this reader: fields it does not know were skipped and are lost
  // on the next save.
  bool newer_minor = false;
};

// Loads a project into an arena. begin() resets the arena's project; step() reads at most `byte_budget`
// bytes from the source per call. On Failed the project is left reset (empty, valid), never half loaded.
// Memory: this object, 3.0 KB, lives wherever the caller puts it (a job slot, a static).
class Reader {
 public:
  Reader() = default;
  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  void begin(core::ByteSource& source, model::ModelArena& arena) noexcept;
  Progress step(std::size_t byte_budget = kDefaultStepBudget) noexcept;
  Progress progress() const noexcept { return progress_; }
  const Report& report() const noexcept { return report_; }

 private:
  enum class State : std::uint8_t {
    FormHeader, ChunkHeader, Version, Payload, PattHead, PattName, PattTrackLen, PattTrackData, UnknownBody, Skip,
    Pad,
    Done, Failed,
  };
  enum class Pull : std::uint8_t { Complete, Partial, Error };

  Pull pull(std::size_t& budget) noexcept;
  void advance() noexcept;
  void fail(Error e, const char* detail) noexcept;
  bool check(model::Status s, const char* what) noexcept;
  bool check_version(std::uint16_t version) noexcept;

  void begin_chunk_header() noexcept;
  void on_chunk_header() noexcept;
  void on_payload() noexcept;
  void end_payload() noexcept;
  void end_chunk() noexcept;
  void enter_skip(std::uint32_t n) noexcept;
  void finish() noexcept;
  void next_track() noexcept;
  void on_patt_head() noexcept;
  bool parse_phdr(std::size_t len) noexcept;
  bool parse_trak(std::size_t len) noexcept;
  bool parse_modr(std::size_t len) noexcept;
  bool parse_song(std::size_t len) noexcept;
  bool parse_inst(std::size_t len) noexcept;
  bool parse_settings(std::size_t len, bool bindings) noexcept;
  bool parse_state(std::size_t len, unsigned part) noexcept;
  bool parse_crc(std::size_t len) noexcept;

  core::ByteSource* source_ = nullptr;
  model::Project* project_ = nullptr;
  State state_ = State::Failed;
  Progress progress_ = Progress::Failed;
  Report report_;
  Crc32 crc_;
  Crc32 crc_before_chunk_;
  std::size_t want_ = 0;  // bytes the current state needs in scratch_
  std::size_t have_ = 0;
  std::uint32_t form_left_ = 0;   // bytes of the FORM after the current chunk
  std::uint32_t chunk_id_ = 0;
  std::uint32_t chunk_len_ = 0;
  std::uint32_t chunk_left_ = 0;  // payload bytes of the current chunk not consumed by a parser yet
  std::uint32_t skip_left_ = 0;
  std::uint32_t expected_patterns_ = 0;
  std::uint32_t expected_instruments_ = 0;
  std::uint32_t expected_modules_ = 0;
  std::uint8_t rank_ = 0;  // rank of the last known chunk (canonical order)
  std::uint8_t chunk_pad_ = 0;
  bool chunk_minor_newer_ = false;  // the chunk's minor version is newer than the one this reader knows for its kind
  std::uint8_t chunk_minor_ = 0;    // the minor version of the chunk being read
  std::uint8_t law_ = 0;            // the LAW chunk's value: held here, applied to the project at Done (cleared in begin())
  std::uint8_t patt_id_ = 0;
  std::uint8_t patt_tracks_ = 0;
  std::uint8_t patt_track_ = 0;
  std::uint8_t patt_name_len_ = 0;
  std::uint8_t row_len_ = 0;
  std::uint8_t scratch_[kScratchBytes];
  model::Step row_[model::kMaxPatternLength];
  model::Instrument inst_;
  model::Settings settings_;  // the project's settings and bindings as the chunks so far say (SETS, then LRNB)
};

// Saves a project to a sink, in the canonical chunk order (PHDR, TRAK, MODR, SONG, PATT by id, INST by slot, SETS when a project setting
// is not its default, LRNB when a binding is in use, MIDI, USBS, MIXR, PERF and APPM for the project-level state the project has (OS-004d), unknown chunks in the order they were read, CRC). The project must not change between begin() and Done:
// step() checks its generation and fails with Error::Modified.
//
// begin() does no work on the project. The first steps run a budgeted sizing pre-pass (the FORM length comes
// before the chunks, so the whole project is measured first; a pattern track costs the steps it scans against
// the step's budget, at least one item is done per call) and write nothing; sized() says when it is over and
// total_bytes() is valid. Memory: this object, 1.4 KB.
class Writer {
 public:
  Writer() = default;
  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;

  // `stereo_mask` (OS-STEREO (b)): bit n lets instrument slot n's stereo flag be written. The app holds a flag back where the sample has no side signal on the card (rule 4 of docs/specs/OS-STEREO.md: a 1 is
  // never saved without its file); the project itself is not touched. The default writes every flag as it is.
  void begin(const model::Project& project, core::ByteSink& sink, std::uint64_t stereo_mask = ~std::uint64_t{0}) noexcept;
  Progress step(std::size_t byte_budget = kDefaultStepBudget) noexcept;
  Progress progress() const noexcept { return progress_; }
  const Report& report() const noexcept { return report_; }
  // True once the sizing pre-pass is over; total_bytes() is the file size from then on (0 before).
  bool sized() const noexcept { return sized_; }
  std::uint32_t total_bytes() const noexcept { return sized_ ? form_len_ + 8u : 0u; }

 private:
  enum class Unit : std::uint8_t { Measure, Form, Phdr, Trak, Modr, Song, Patt, Inst, Sets, Lrnb, State, Unknown, Crc, End };
  enum class Phase : std::uint8_t { Fixed, Patterns, Instruments, UnknownChunks, Done };

  bool measure(std::size_t& budget) noexcept;
  bool build_piece() noexcept;
  void fail(Error e, const char* detail) noexcept;

  bool stereo_written(std::uint32_t slot) const noexcept {
    const model::Instrument* in = project_->instrument(slot);
    return in != nullptr && in->stereo != 0 && slot < 64u && ((stereo_mask_ >> slot) & 1u) != 0u;
  }

  const model::Project* project_ = nullptr;
  std::uint64_t stereo_mask_ = ~std::uint64_t{0};
  core::ByteSink* sink_ = nullptr;
  Progress progress_ = Progress::Failed;
  Report report_;
  Crc32 crc_;
  Unit unit_ = Unit::End;
  Phase phase_ = Phase::Done;
  bool sized_ = false;
  std::uint32_t generation_ = 0;
  std::uint32_t form_len_ = 0;
  std::uint32_t built_ = 0;     // bytes of pieces built so far
  std::uint32_t cursor_ = 0;    // pattern id / instrument slot / state part / unknown chunk index being written
  std::uint32_t sub_ = 0;       // piece index inside a pattern, byte offset inside an unknown chunk
  std::uint32_t measure_total_ = 0;    // sizing pre-pass: FORM length so far, and where it is
  std::uint32_t measure_id_ = 0;
  std::uint32_t measure_track_ = 0;
  std::size_t measure_payload_ = 0;
  std::uint32_t patt_sized_ = 0;       // patterns sized, in ascending id order
  std::uint32_t patt_written_ = 0;     // patterns whose chunk has been started
  std::size_t piece_len_ = 0;
  std::size_t piece_off_ = 0;
  std::size_t patt_payload_ = 0;       // payload of the PATT chunk being written
  std::uint16_t patt_payload_of_[model::kMaxPatterns] = {};  // payload of each sized PATT chunk
  std::uint8_t scratch_[kScratchBytes];
};

// OS-QAFIX-4 (finding 4): checks a project file's framing before the Reader touches the document, so that a file that cannot load leaves the document as it is (the Reader resets the project at begin()
// and a second project does not fit the SDRAM). It reads the stream once, never touches a project, and holds no copy of it: the FORM header, every chunk's header and length against the FORM, the order
// of the known chunks, each known chunk's version (a major newer than this reader's) and length bounds (the Reader's own), the PHDR counts against the model's limits and against the PATT and INST chunks
// that follow, the pad bytes, and the CRC chunk's CRC-32 over everything before it. It is never stricter than the Reader (the same tables, in this file): a file the Reader loads, it accepts. What it does
// not see is the content of the chunks (a field out of range, a duplicate pattern id, a model invariant): the Reader still fails those after its reset, as before. Memory: this object, about 0.7 KB.
class Verifier {
 public:
  static constexpr std::size_t kScratch = 512;  // holds the FORM header (12), a chunk header (8), the PHDR (kPhdrMax) and the CRC chunk (6); a skip reads blocks of it
  Verifier() = default;
  Verifier(const Verifier&) = delete;
  Verifier& operator=(const Verifier&) = delete;

  void begin(core::ByteSource& source) noexcept;
  Progress step(std::size_t byte_budget = kDefaultStepBudget) noexcept;
  Progress progress() const noexcept { return progress_; }
  const Report& report() const noexcept { return report_; }  // error, detail, offset, bytes, chunk; patterns and instruments count the chunks seen

 private:
  enum class State : std::uint8_t { FormHeader, ChunkHeader, Version, Payload, Skip, Pad, Done, Failed };

  bool pull(std::size_t& budget) noexcept;  // false: Partial or failed (progress_ says)
  void advance() noexcept;
  void fail(Error e, const char* detail) noexcept;
  void begin_chunk_header() noexcept;
  void on_chunk_header() noexcept;
  void on_payload() noexcept;
  void enter_skip(std::uint32_t n) noexcept;
  void end_payload() noexcept;
  void end_chunk() noexcept;
  void finish() noexcept;

  core::ByteSource* source_ = nullptr;
  State state_ = State::Failed;
  Progress progress_ = Progress::Failed;
  Report report_;
  Crc32 crc_;
  Crc32 crc_before_chunk_;
  std::size_t want_ = 0;
  std::size_t have_ = 0;
  std::uint32_t form_left_ = 0;
  std::uint32_t chunk_id_ = 0;
  std::uint32_t chunk_len_ = 0;
  std::uint32_t skip_left_ = 0;
  std::uint32_t expected_patterns_ = 0;
  std::uint32_t expected_instruments_ = 0;
  std::uint8_t rank_ = 0;
  std::uint8_t chunk_pad_ = 0;
  bool chunk_minor_newer_ = false;
  std::uint8_t chunk_minor_ = 0;
  std::uint8_t scratch_[kScratch];
};

}  // namespace o3os::formats::native
