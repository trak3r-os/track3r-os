// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/core/byte_io.h>
#include <o3os/formats/crc32.h>
#include <o3os/formats/format.h>
#include <o3os/model/project.h>

#include <cstddef>
#include <cstdint>

namespace o3os::formats::og {

// The Tracker OG's own files (OS-004b, ADR 0006): project.mt, pattern_NN.mtp, instrument_NN.mti, NN name.pti and
// patterns/patternsMetadata ("PAMD"), as the fixtures hold them (project versions 2 to 16), read into the model and
// written back in the layouts of project v16, 12-track pattern v5, .pti v9 and PAMD v1 (whether an official device opens an exported project is an open research question, unverified: no device was tried). The byte-level
// mapping, with what is kept, normalised and dropped, is docs/formats/og.md; the layouts are the PAR-006, 007,
// 005-004 and 008-009 specs.
//
// An OG project is a folder, so an import is several small reads into one project, in this order: the project
// file (resets the arena's project, creates the 16 MIDI instruments of slots 49-64), then each pattern, each
// instrument, and the pattern names last. Every reader is a resumable FormatReader (begin / step / report) that
// does no file I/O, allocates nothing and trusts no length; any input, however hostile, ends in Done or in a typed
// error. The files other than the project file add to the project and undo their own changes on failure.
//
// Versions (ADR 0006 amendment 1): a version newer than the highest with evidence (project 17, pattern 6,
// instrument 9, PAMD 1), or one in a gap nobody has seen (project 8-10 and 15), is read as far as its layout is
// known to be unchanged, reported as `unverified_version`, and never guessed at: see docs/formats/og.md.
// Project v17 (official 1.9.2, OS-004c) is a third class: no file of it has been seen, so its layout is an open research
// question (unverified, docs/formats/og.md section 2). It is read like v16 (`observed_only`, not
// `unverified_version`) and written when Export targets 1.9.2.

inline constexpr std::uint8_t kProjectVerified = 16;    // the highest project version with fixture evidence
inline constexpr std::uint8_t kProjectObservedOnly = 17;  // no file backs it (open research question, unverified; assumed to be official 1.9.2's)
inline constexpr std::uint8_t kPatternVerified = 6;  // OS-CEPAT S1: v6 (tracker-ce, from the source: tracker-ce@7fd6c2c) is read
inline constexpr std::uint8_t kInstrumentVerified = 9;
inline constexpr std::uint8_t kPamdVerified = 1;

inline constexpr std::uint32_t kProjectImageMax = 2400;  // the longest project.mt read (2,324 B, v17, is the longest known)
inline constexpr std::uint32_t kProjectBytes16 = 2320;
inline constexpr std::uint32_t kProjectBytes17 = 2324;
// OS-QAFIX-4: the framing test of the project reader (ProjectReader::finish) without a model: `head` is the first `n` bytes of a project.mt of `size` bytes (16 are enough). True when the reader
// would not refuse the file for its size, magic or version (a known version has exactly its layout's length; an unknown one at least the shared part), so a load can be refused before it
// replaces the document. Never stricter than the reader. `why` (may be null) gets the reader's words for a refusal.
bool project_frame_ok(const std::uint8_t* head, std::size_t n, std::uint32_t size, const char** why = nullptr) noexcept;
inline constexpr std::uint32_t kPatternHead = 28;
inline constexpr std::uint32_t kPatternRow = 769;    // v1..v5: length byte + 128 steps of 6 bytes (8, 12 or 16 rows)
inline constexpr std::uint32_t kPatternBytes8 = 6184;   // v1..v5, 8 tracks
inline constexpr std::uint32_t kPatternBytes12 = 9260;  // v1..v5, 12 tracks (firmware 1.9)
// v5, 16 tracks (tracker-ce reads it; OS-CEPAT S1b, from the source: tracker-ce@7fd6c2c:core/include/patternFileValidation.h:12, 26-29)
inline constexpr std::uint32_t kPatternBytes16 = 12336;
// v6 (OS-CEPAT S1, tracker-ce@7fd6c2c:core/source/mtSequencer.h:282-336, from the source): length byte + 128 steps
// of 8 bytes per row, 28 + n * 1,025 + 4 bytes for n = 8, 12 or 16 tracks; the size u16 at offset 12 says which.
inline constexpr std::uint32_t kPatternRow6 = 1025;
inline constexpr std::uint32_t kPatternRowMax = kPatternRow6;
inline constexpr std::uint32_t kPatternBytes6_8 = 8232;    // 28 + 8 * 1,025 + 4
inline constexpr std::uint32_t kPatternBytes6_12 = 12332;  // 28 + 12 * 1,025 + 4
inline constexpr std::uint32_t kPatternBytes6_16 = 16432;  // 28 + 16 * 1,025 + 4
// The track count a v6 declared size gives: 8, 12 or 16, or 0 when the size is not one of the three v6 sizes.
inline constexpr std::uint32_t pattern_tracks_v6(std::uint32_t size) noexcept {
  return size == kPatternBytes6_8 ? 8u : size == kPatternBytes6_12 ? 12u : size == kPatternBytes6_16 ? 16u : 0u;
}
inline constexpr std::uint32_t kInstrumentBytes = 392;  // header + sampler struct + CRC: a whole .mti, the head of a .pti
inline constexpr std::uint32_t kInstrumentBytesLegacy = 388;  // versions 1 to 3
inline constexpr std::uint32_t kPamdHeader = 16;
inline constexpr std::uint32_t kPamdRecord = 50;
inline constexpr std::uint32_t kPamdRecords = 256;  // the one PAMD fixture holds 256 records
inline constexpr std::uint32_t kPamdBytes = kPamdHeader + kPamdRecord * kPamdRecords;  // 12,816: the longest file read
inline constexpr std::uint32_t kMaxPcmFrames = 5898240;  // one instrument may fill the whole sample memory

inline constexpr std::uint32_t kTracks = 12;        // a v16 project and a v5 pattern hold 12 tracks; tracks 9-12 are MIDI tracks in the OG (inferred, docs/formats/og.md section 10)
inline constexpr std::uint32_t kSampleSlots = 48;   // instrument slots 0..47 are files; slots 48..63 are the 16 MIDI channels
inline constexpr std::uint32_t kMidiFirstSlot = 48;

// Groups of project.mt fields that have no field in the model. The settings groups (all but session and pattern
// bitmask) are kept verbatim in the OGPM chunk when they differ from the defaults Export writes: FormatReport::
// kept_groups says which, dropped_groups lists the groups that were not kept (session state, the bitmask, which
// Export rebuilds; layouts this reader cannot read; a pool too full for the chunk).
enum DropGroup : std::uint32_t {
  kDropSession = 1u << 0,        // last used note / instrument / volume / FX, current pattern, song position, pad board, edit step
  kDropDelay = 1u << 1,          // delay feedback, time, sync and ping-pong
  kDropLimiter = 1u << 2,        // limiter attack, release, threshold; bit depth
  kDropReverb = 1u << 3,         // reverb size, damping, pre-delay, diffusion
  kDropInputs = 1u << 4,         // input source, gains, monitor, radio frequency
  kDropPerformance = 1u << 5,    // performance mode pages and values
  kDropMidiVelocity = 1u << 6,   // per-MIDI-channel velocity (versions before 1.3.0)
  kDropPatternBitmask = 1u << 7, // which patterns hold notes: rebuilt from the patterns on export
  kDropExt13 = 1u << 8,          // the 1.3 to 1.6 block: EQ, per-channel MIDI records, delay filter
  kDropExt19 = 1u << 9,          // the 1.7 to 1.9 block: settings of unknown layout
};
inline constexpr unsigned kDropGroupCount = 10;
const char* drop_group_name(unsigned index) noexcept;  // "session", "delay"...; "" past the last

// Which official firmware generation Export -> OG writes the project file for (WriteParams::target). 1.9.0 is the default
// and writes project v16 (2,320 B); 1.9.2 writes v17 (2,324 B), which is expected to name the project on a 1.9.2 device where v16 may not
// (open research question, unverified: no device was tried; docs/formats/og.md section 3). The pattern, instrument and PAMD writers are the same for both and ignore it.
inline constexpr std::uint8_t kTargetOg190 = 0;
inline constexpr std::uint8_t kTargetOg192 = 1;

// Two chunks the OG formats keep in the model's pool of unknown chunks, so that a native project carries what an OG
// project held and the model has no field for (docs/formats/og.md, o3-project.md): OGPM, the project settings (the
// groups of DropGroup that are real settings: not the session state, not the pattern bitmask), and OGPN, the PAMD
// file as it was read. Both are versioned (0x0100) and opaque to the core. OGPM 1.1 (0x0101) widens group 9 by the
// byte at 0x80C of a v17 project, a field assumed to belong to official 1.9.2 (open research question, unverified) whose meaning is not known; a chunk that carries no such
// byte (a zero one is not worth carrying) stays 1.0.
inline constexpr std::uint32_t kChunkSettings = 0x4F47504Du;  // "OGPM"
inline constexpr std::uint32_t kChunkNames = 0x4F47504Eu;     // "OGPN"
// True when the payload of an OGPM chunk is valid; `groups` then has the DropGroup bits it carries, and `v17_byte` (when
// given) the byte of 0x80C that an OGPM 1.1 chunk holds (0 when it holds none).
bool inspect_settings(const std::uint8_t* payload, std::uint32_t length, std::uint16_t& groups,
                      std::uint8_t* v17_byte = nullptr) noexcept;
// True when the payload of an OGPN chunk is valid; `file_length` then is the length of the PAMD file it holds.
bool inspect_names(const std::uint8_t* payload, std::uint32_t length, std::uint32_t& file_length) noexcept;

// What a read says beyond FormatReport (whose `dropped_groups` holds DropGroup bits for a project file, and
// `sanitised` counts the values the model cannot hold that were replaced by a default or clamped).
struct ImportReport : FormatReport {
  std::uint8_t version = 0;       // the structure version byte of the file (0 until it was read)
  bool observed_only = false;    // project v17: a layout with no file of it to check (open research question), read like v16 (not unverified_version)
  std::uint8_t tracks = 0;        // pattern: 8, 12 or 16
  std::uint8_t start_pattern = 0; // project: the pattern the project opens on, the project file's saved-pattern field (0x114), at least 1; 0 until read (OS-007j)
  std::uint32_t names_applied = 0;  // PAMD: pattern names set
  std::uint32_t names_skipped = 0;  // PAMD: names of patterns that are not in the project
  bool partial_record = false;    // PAMD: bytes after the last whole record, ignored
  bool names_kept = false;        // PAMD: the file was kept verbatim (OGPN) for Export; false: the pool had no room
  // OS-CEPAT S1 (A10), pattern v6 and the id map: what the new columns and the cleared ids cost.
  std::uint32_t micromoves_dropped = 0;  // micromove values with no FX column free for them, or with Random value 16 in the other one
  std::uint32_t values_clamped = 0;      // volume values above 100, read as full volume (tracker-ce never writes them)
  std::uint32_t ce_fx_cleared = 0;       // OG FX ids 75..127, cleared to (none, 0) by the id map of 3.1 (OS-CEPAT S1b)
};

namespace detail {

// Shared plumbing of the readers: the source, the project, the pull of bytes within a budget, the CRC, the report.
class ReaderBase : public FormatReader {
 public:
  Progress progress() const noexcept override { return progress_; }
  const FormatReport& report() const noexcept override { return report_; }
  const ImportReport& import_report() const noexcept { return report_; }

 protected:
  enum class Pull : std::uint8_t { Complete, Partial, End, Error };

  void begin_base(core::ByteSource& source, model::ModelArena& arena) noexcept;
  // Reads into dst[have..want) within `budget`. Complete: have == want. Partial: budget spent or the source
  // would block. End: the source is over (have < want). Error: fail() has been called.
  Pull pull(std::uint8_t* dst, std::size_t want, std::size_t& have, std::size_t& budget, bool crc) noexcept;
  void fail(FormatError e, const char* detail) noexcept;

  core::ByteSource* source_ = nullptr;
  model::Project* project_ = nullptr;
  Progress progress_ = Progress::Failed;
  ImportReport report_;
  Crc32 crc_;
};

}  // namespace detail

// project.mt (versions 2-7 at their native lengths, 11-14 at 1,572 B, 16 at 2,320 B, 17 at 2,324 B; v1 is refused, 8-10, 15
// and 18+ are read as far as the layout is common to all versions). Resets the project, then sets its name, tempo,
// default pattern length, song, 16 track slots, master section, and the 16 MIDI instruments, and keeps the settings
// the model has no field for (delay, limiter, reverb, inputs, performance, the 1.3 and 1.7 blocks, MIDI velocity, the
// byte at 0x80C of v17) in an OGPM chunk when they are not the defaults Export would write. Memory: 2.5 KB (MK66).
class ProjectReader final : public detail::ReaderBase {
 public:
  void begin(core::ByteSource& source, model::ModelArena& arena, const ReadParams& params) noexcept override;
  Progress step(std::size_t byte_budget = kDefaultStepBudget) noexcept override;

 private:
  void finish() noexcept;
  bool apply(std::uint8_t version, std::uint32_t len) noexcept;
  bool keep_settings(std::uint32_t stored, unsigned version) noexcept;

  std::size_t have_ = 0;
  bool probing_ = false;
  std::uint8_t image_[kProjectImageMax];
};

// pattern_NN.mtp: 8 tracks (6,184 B), 12 tracks (9,260 B, firmware 1.9) or 16 tracks (12,336 B, tracker-ce's v5;
// versions 1 to 5, the stream tells the track count, OS-CEPAT S1b), or v6 (8, 12 or 16 tracks, 8,232 / 12,332 /
// 16,432 B, tracker-ce: the declared size at offset 12 gives the layout, OS-CEPAT S1; versions above 6 with a v6
// size are read the same way and flagged unverified_version). Creates the pattern `params.index` (1..255), one
// track row at a time. Memory: 2.1 KB (MK66, in the SDRAM slack window, see platform/mk66/src/app_main.cpp).
class PatternReader final : public detail::ReaderBase {
 public:
  void begin(core::ByteSource& source, model::ModelArena& arena, const ReadParams& params) noexcept override;
  Progress step(std::size_t byte_budget = kDefaultStepBudget) noexcept override;

 private:
  enum class State : std::uint8_t { Head, Row, Tail, Tail12, Crc, End, Done, Failed };

  void reject(FormatError e, const char* detail) noexcept;
  void on_head() noexcept;
  void on_row() noexcept;
  void finish() noexcept;

  State state_ = State::Failed;
  std::size_t want_ = 0;
  std::size_t have_ = 0;
  std::uint8_t id_ = 0;
  bool created_ = false;
  bool v6_ = false;               // version >= 6: the declared size at offset 12 fixed the layout (OS-CEPAT S1)
  std::uint8_t tracks_total_ = 0;  // v6: the track count the declared size gives
  std::uint8_t track_ = 0;
  std::uint8_t first_length_ = 0;
  std::uint32_t crc8_ = 0;   // the CRC after the first eight tracks: it is the file's CRC when the file ends there
  std::uint32_t crc12_ = 0;  // the same after twelve tracks (OS-CEPAT S1b: a 16-track v5 file goes on, a 12-track one ends here)
  std::uint8_t scratch_[kPatternRowMax];
  model::Step row_[model::kMaxPatternLength];
};

// instrument_NN.mti (versions 1-9, 388 or 392 B, CRC enforced) and NN name.pti (versions 4-9, 392 B + mono PCM,
// CRC only warned about). Sets the sample instrument `params.index` (0..47). The PCM of a .pti is not read into
// anything: the report says how many frames follow, and the platform copies them (offset 392 of the file) into
// the WAV `samples/instrNN.wav` that the instrument's sample path names. Memory: 1.5 KB (MK66).
class InstrumentReader final : public detail::ReaderBase {
 public:
  explicit InstrumentReader(bool pti) noexcept : pti_(pti) {}
  void begin(core::ByteSource& source, model::ModelArena& arena, const ReadParams& params) noexcept override;
  Progress step(std::size_t byte_budget = kDefaultStepBudget) noexcept override;
  // ReadParams::standalone: the instrument that was decoded (valid once the read is Done with report().instruments == 1).
  const model::Instrument& instrument() const noexcept { return inst_; }

 private:
  enum class State : std::uint8_t { Head, Rest, Probe, Pcm, Done, Failed };

  void on_head() noexcept;
  void on_image() noexcept;
  void decode() noexcept;
  void on_end() noexcept;

  bool pti_;
  bool standalone_ = false;
  State state_ = State::Failed;
  std::size_t want_ = 0;
  std::size_t have_ = 0;
  std::uint16_t slot_ = 0;
  std::uint32_t size_hint_ = 0;
  std::uint32_t pcm_bytes_ = 0;
  std::uint8_t version_ = 0;
  std::uint8_t image_[kInstrumentBytes];
  std::uint8_t drain_[256];
  model::Instrument inst_;
};

// patterns/patternsMetadata ("PAMD" version 1): up to 256 records of 50 bytes, record i naming pattern i + 1 (an
// inference, see docs/formats/og.md). The whole file is read and checked before anything is applied (so a bad file
// changes nothing), then the names of the patterns already in the project are set and the file is kept verbatim in an
// OGPN chunk for Export. A project that already holds an OGPN chunk is refused (read the project file first, which
// resets it). Memory: 12.9 KB (MK66).
class PamdReader final : public detail::ReaderBase {
 public:
  void begin(core::ByteSource& source, model::ModelArena& arena, const ReadParams& params) noexcept override;
  Progress step(std::size_t byte_budget = kDefaultStepBudget) noexcept override;

 private:
  void finish() noexcept;

  std::size_t have_ = 0;
  bool probing_ = false;
  std::uint8_t file_[kPamdBytes];
};

// ---- export ---------------------------------------------------------------------------------------------

// What Export -> OG leaves out or rewrites, summed over the files of one export (the app adds each writer's
// `drops` together). Everything here is counted, nothing is silent (ADR 0006).
struct ExportDrops {
  std::uint32_t tracks_beyond_12 = 0;      // steps that hold something in tracks 13-16 (OG 1.9 has 12 tracks)
  std::uint32_t engine_fx = 0;             // FX columns with an engine FX type (128..255), written as none
  std::uint32_t core_fx = 0;               // FX columns with a core FX type (43..95, ADR 0004), written as none
  std::uint32_t ce_fx = 0;                 // FX columns with a tracker-ce FX type (96..127 = OG id + 53, ADR 0004 amendment 3), written as none
  std::uint32_t volume_as_fx18 = 0;        // volume columns written as FX 18 (kept, but no longer a column)
  std::uint32_t volume_dropped = 0;        // volume columns that could not be written: both FX slots taken, or FX 18 there
  std::uint32_t track_settings = 0;        // tracks with a pan, solo, output bus or external-audio type: OG has none
  std::uint32_t audio_on_midi_tracks = 0;  // audio tracks 9-12, which the OG has as MIDI tracks (inferred): written there as they are (a note, not a loss)
  std::uint32_t track_names_cut = 0;       // track names longer than the OG field (20 for tracks 1-8, 7 for 9-16)
  std::uint32_t project_settings = 0;      // swing that is not 50: OG has no project swing
  std::uint32_t module_instruments = 0;    // module instruments: not exportable
  std::uint32_t midi_instruments = 0;      // MIDI instruments outside OG's fixed slots 49-64: not exportable
  std::uint32_t sample_slots_beyond_48 = 0;  // sample instruments in slots 49-64: OG has 48 sample slots
  std::uint32_t unknown_chunks = 0;        // native chunks this version keeps for a newer one: no OG home
  std::uint32_t instrument_names_cut = 0;  // instrument names longer than the OG's 31 characters
  std::uint32_t lfo_sine = 0;              // OS-LFOSINE: sample instruments with an LFO of the Sine shape (OG has five shapes): written as Triangle
  std::uint32_t wt_mod = 0;                // OS-CEWTENV W1: WT slots of a sample instrument (an envelope loop of 2 or 3; OG 1.9 has no WT modes): written off (loop 0, enable 0, the shape held to 0..4, the warp and sustain bytes zero), one per slot
  std::uint32_t pattern_names = 0;         // patterns with a name that is not the one in the PAMD file Export writes back
  std::uint32_t settings_ignored = 0;      // OGPM / OGPN chunks that were not valid (or repeated) and so were not used
  // OS-FILES-C C2b (docs/specs/OS-FILES-C.md section 4): the PCM stage and the levels, counted by app/og_export.h when it plans the sample instruments (no writer counts them).
  std::uint32_t midi_settings = 0;         // implicit-slot MIDI instruments with a non-default INST 1.3 field (program, port, bend range, offset, CC slots, velocity): the OG keeps the channel only
  std::uint32_t resampled = 0;             // sample instruments written resampled to 44,100 Hz (another recorded rate, or the Amiga engine)
  std::uint32_t rate_ignored = 0;          // wavetable instruments with a recorded rate other than 44,100: written as they are, the rate dropped
  std::uint32_t amiga_engine = 0;          // instruments on the Amiga engine: the engine (its 8-bit rounding and period clamp) is gone in the file
  std::uint32_t bit_engine = 0;            // instruments on the 12 BIT or 8 BIT engine: a .pti cannot say it, so the instrument is not written
  std::uint32_t pcm_too_long = 0;          // resampled audio longer than a .pti holds: the instrument is not written
  std::uint32_t instruments_not_written = 0;  // sample instruments in slots 1-48 with no .pti: no audio, or amiga_engine in a mode Paula does not play, bit_engine, pcm_too_long, no_sample_memory
  std::uint32_t no_sample_memory = 0;      // the sample memory was not up when the plan was made (no bank): every sample instrument counted in instruments_not_written for this reason, not for having no audio
  std::uint32_t levels_converted = 0;      // levels rewritten from the 1.1.14 law to the 1.2+ law so that the gain stays
  std::uint32_t levels_clamped = 0;        // 1.1.14 levels under -24 dB, written as level 1 (the quietest the 1.2+ law has): louder than the instrument plays
  std::uint32_t stereo_mid = 0;            // OS-STEREO (e): stereo instruments written as their mid M (a mono .pti, the same bytes as a mono sample's): the side S is not in the file
  // project: DropGroup bits of every group written as a default because the project carries no value for it (the
  // session state always; a settings group the OGPM chunk does not hold). Not a loss: the model has nothing there.
  std::uint32_t defaulted_groups = 0;

  void add(const ExportDrops& other) noexcept;
  // Anything lost (volume_as_fx18 is a rewrite, audio_on_midi_tracks a note and defaulted_groups a default, not losses).
  bool any_lost() const noexcept;
};

struct ExportReport : FormatReport {
  ExportDrops drops;
};

namespace detail {

class WriterBase : public FormatWriter {
 public:
  Progress progress() const noexcept override { return progress_; }
  const FormatReport& report() const noexcept override { return report_; }
  const ExportReport& export_report() const noexcept { return report_; }

 protected:
  enum class Flush : std::uint8_t { Done, Partial, Error };

  void begin_base(const model::Project& project, core::ByteSink& sink) noexcept;
  void fail(FormatError e, const char* detail) noexcept;
  // Writes buf[off..len) within `budget`; Done when all of it is out.
  Flush flush(const std::uint8_t* buf, std::size_t len, std::size_t& off, std::size_t& budget) noexcept;
  // False (and Modified) when the project changed since begin().
  bool unchanged() noexcept;

  const model::Project* project_ = nullptr;
  core::ByteSink* sink_ = nullptr;
  Progress progress_ = Progress::Failed;
  ExportReport report_;
  Crc32 crc_;
  std::uint32_t generation_ = 0;
};

}  // namespace detail

// project.mt version 16 (2,320 B, `WriteParams::target` kTargetOg190, the default) or version 17 (2,324 B, kTargetOg192):
// name, tempo, default pattern length, song, tracks 1-16 (names, mute, volume),
// master volumes and mutes from the model; the pattern-usage bitmask rebuilt from the patterns (so write the
// project file after the patterns are in the model); the settings groups from the project's OGPM chunk and, where it
// holds none, at the documented default (`drops.defaulted_groups` says which). The byte at 0x80C that a v17 project
// carries (OGPM 1.1) is written to a v17 file and counted in `drops.project_settings` for a v16 one. Any other target
// is `Malformed`. Memory: 2.7 KB (MK66).
class ProjectWriter final : public detail::WriterBase {
 public:
  void begin(const model::Project& project, core::ByteSink& sink, const WriteParams& params) noexcept override;
  Progress step(std::size_t byte_budget = kDefaultStepBudget) noexcept override;

 private:
  enum class Phase : std::uint8_t { Scan, Build, Emit, Done };

  void build() noexcept;

  Phase phase_ = Phase::Done;
  std::uint8_t target_ = kTargetOg190;
  std::uint32_t scan_id_ = 0;  // next pattern id to examine for the bitmask
  std::size_t off_ = 0;
  std::uint8_t bitmask_[255];
  std::uint8_t image_[kProjectBytes17];  // the file is the first kProjectBytes16 (target 1.9.0) or kProjectBytes17 bytes
};

// pattern_NN.mtp, 12 tracks, version 5 (9,260 B): the pattern `params.index`, tracks 1-12. Volume columns become
// FX 18, engine FX are cleared, tracks 13-16 are left out, all counted in `drops`. Memory: 0.9 KB (MK66).
class PatternWriter final : public detail::WriterBase {
 public:
  void begin(const model::Project& project, core::ByteSink& sink, const WriteParams& params) noexcept override;
  Progress step(std::size_t byte_budget = kDefaultStepBudget) noexcept override;

 private:
  void build_piece() noexcept;

  std::uint8_t id_ = 0;
  std::uint8_t piece_ = 0;   // 0 head, 1..12 tracks, 13 CRC, 14 done
  bool built_ = false;
  std::size_t len_ = 0;
  std::size_t off_ = 0;
  std::uint8_t scratch_[kPatternRow];
};

// NN name.pti, version 9: the sample instrument in slot `params.index` (0..47) followed by `params.pcm_frames`
// frames of mono PCM from `params.pcm`; without PCM the file is the 392-byte head alone (the shape of an .mti).
// Memory: 1.0 KB (MK66).
class InstrumentWriter final : public detail::WriterBase {
 public:
  void begin(const model::Project& project, core::ByteSink& sink, const WriteParams& params) noexcept override;
  Progress step(std::size_t byte_budget = kDefaultStepBudget) noexcept override;

 private:
  enum class Phase : std::uint8_t { Head, Pcm, Done };

  void build() noexcept;

  Phase phase_ = Phase::Done;
  core::ByteSource* pcm_ = nullptr;
  std::uint16_t slot_ = 0;
  std::uint32_t frames_ = 0;
  std::uint32_t pcm_left_ = 0;  // bytes still to pull from the source
  std::int16_t level_ = -1;     // WriteParams::level_override
  std::int16_t fine_ = WriteParams::kNoFineOverride;  // WriteParams::fine_override
  std::size_t off_ = 0;
  std::size_t pcm_have_ = 0;    // bytes in pcm_buf_
  std::size_t pcm_off_ = 0;     // of them already written
  std::uint8_t image_[kInstrumentBytes];
  std::uint8_t pcm_buf_[512];
};

// patterns/patternsMetadata: the PAMD file the project was read from, byte for byte (the OGPN chunk), or, for a
// project that has none, a header and 256 empty records (12,816 bytes). Names given to patterns in the OS are NOT
// written until the record <-> pattern mapping is confirmed on an official device (docs/formats/og.md section 6):
// they are counted in `drops.pattern_names`. Memory: 0.2 KB (MK66).
class PamdWriter final : public detail::WriterBase {
 public:
  void begin(const model::Project& project, core::ByteSink& sink, const WriteParams& params) noexcept override;
  Progress step(std::size_t byte_budget = kDefaultStepBudget) noexcept override;

 private:
  const std::uint8_t* head_ = nullptr;  // the file's bytes up to its last non-zero one
  std::uint32_t head_len_ = 0;
  std::uint32_t total_ = 0;             // the file's length: zeros follow the head
  std::size_t off_ = 0;
  std::uint32_t zeros_done_ = 0;
  std::uint8_t header_[kPamdHeader];
};

// ---- instruments and file names ---------------------------------------------------------------------------

// What Export -> OG does with the instrument in a model slot.
enum class ExportClass : std::uint8_t {
  Empty,         // no instrument
  Pti,           // a sample instrument in slots 0..47: write NN name.pti (InstrumentWriter)
  ImplicitMidi,  // MIDI channel n in slot 47 + n: OG has these without a file
  Module,        // dropped: no OG equivalent
  MidiMoved,     // dropped: a MIDI instrument anywhere else
  SlotBeyond48,  // dropped: a sample instrument in slots 48..63
};
ExportClass export_class(const model::Project& project, std::uint32_t slot) noexcept;
// Adds the instruments Export cannot write (module, moved MIDI, sample beyond slot 48) to `drops`.
void count_instrument_drops(const model::Project& project, ExportDrops& drops) noexcept;

enum class FileClass : std::uint8_t { None, Project, Pattern, Mti, Pti, PatternNames };
struct FileName {
  FileClass file_class = FileClass::None;
  std::uint16_t index = 0;  // pattern id 1..255, or instrument slot 0..47
};
// Recognises an OG folder's files by name, case-insensitively and ignoring a leading path: "project.mt",
// "pattern_NN.mtp" (NN 1..255), "instrument_NN.mti" (NN 1..48, slot NN - 1), "NN name.pti" (1-3 digits, a space,
// anything, ".pti"; NN 1..48), "instrument_NN.pti" (the name in the Workspace of firmware 1.5+), "patternsMetadata". Anything else, including macOS "._" files, is None.
FileName classify_file_name(const char* name) noexcept;
// "NN name.pti" for slot 0..47 into dst (NUL-terminated): the name is cut to fit `cap` and loses every character a
// file name must not carry ('/', '\\', ':', control characters, '"', '*', '?', '<', '>', '|' become '_'), so a
// hostile instrument name can never leave its folder. Returns the length, 0 when `cap` is under 9 or the slot is
// out of range.
std::size_t format_pti_name(char* dst, std::size_t cap, std::uint32_t slot, const char* name) noexcept;

}  // namespace o3os::formats::og
