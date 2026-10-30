// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/core/byte_io.h>
#include <o3os/formats/progress.h>
#include <o3os/model/project.h>

#include <cstddef>
#include <cstdint>

namespace o3os::formats {

namespace med { class RandomSource; }  // formats/med/mmd.h

// Formats as Strategy + registry (ADR 0009 rev 1, OS-004b). A format is a FormatReader and/or a FormatWriter:
// resumable begin / step / report over a ByteSource or ByteSink, no file I/O, no heap, nothing that blocks.
// The interfaces are virtual, which is allowed at format granularity (one call per step() of a job, never
// inside a loop over samples or steps). A FormatRegistry is a small table that finds a format by id, by file
// name (extension) or by sniffed magic, so a file browser, an importer job and a module that adds MOD / XM /
// IT / MIDI all meet in one place.
//
// Contexts: main loop only (jobs, OS-002c). A strategy object is single-use at a time: begin() again only
// after Done or Failed. Its memory is the object's own; the caller owns it (a static, a job slot).

inline constexpr std::size_t kDefaultStepBudget = 4096;  // bytes moved per step() unless the job says otherwise

// Typed errors common to every format, the shape of the native format's list. The detail text of the report
// says which rule was broken.
enum class FormatError : std::uint8_t {
  None,
  Truncated,           // the source ended inside a structure
  BadMagic,            // not a file of this format
  BadCrc,              // a CRC this format enforces does not match
  UnsupportedVersion,  // a version older or newer than anything this reader can interpret (native: newer major)
  Unsupported,         // a valid file that holds something this reader does not take (stereo .pti, 16 tracks...)
  LimitExceeded,       // more than the model holds, or longer than the format allows
  Malformed,           // inconsistent lengths, a value out of range, a duplicate, a bad parameter...
  SourceFailed,        // the byte source reported a failure or broke its contract
  SinkFailed,          // the byte sink reported a failure or broke its contract
  Modified,            // writers: the project changed while it was being written
  Internal,            // a bug, never input
  Cancelled,           // readers that can be abandoned (FormatReader::can_cancel): cancel() was called
};

const char* format_error_name(FormatError e) noexcept;

// What every reader and writer says about a run. Formats with more to tell (the OG import report) extend it.
struct FormatReport {
  FormatError error = FormatError::None;
  const char* detail = "";           // static text: what was wrong, "" when nothing was
  std::uint32_t offset = 0;          // bytes consumed (reader) or produced (writer) when the error was found
  std::uint32_t bytes = 0;           // bytes consumed / produced so far
  // The file is of a version newer than the highest this reader has evidence for (ADR 0006 amendment 1): what
  // is known was read, nothing was guessed, and the UI tells the user. Treat the source as read-only.
  bool unverified_version = false;
  // A CRC that this format only warns about did not match (the data was used).
  bool crc_mismatch = false;
  std::uint32_t patterns = 0;        // patterns applied to the model (readers)
  std::uint32_t instruments = 0;     // instruments applied to the model (readers)
  // Readers: values the model cannot hold, replaced by a default or clamped, and groups of the file's fields that
  // have no place in the model and were not kept (a bit set the format defines; FormatDesc::group_name names the
  // bits). ADR 0006: what is dropped is listed, never silent.
  std::uint32_t sanitised = 0;
  std::uint32_t dropped_groups = 0;
  // Readers: groups of the file's fields that the model has no place for but keeps verbatim in an opaque chunk of
  // its own, so the same format can write them back (the OG project settings, OS-004b). Same bits as dropped_groups.
  std::uint32_t kept_groups = 0;
  // Readers of sample formats (.pti): mono frames of PCM after the header, which the platform copies into the WAV
  // the instrument names (0: none, and the slot stays empty).
  std::uint32_t pcm_frames = 0;
  // Readers of sample formats (.wav, OS-002c): the frames written to ReadParams::pcm and their channel count (1 or 2).
  // Nothing was written when pcm_frames is 0. The rate, loop and slices went into the sample instrument.
  std::uint8_t pcm_channels = 0;
  // OS-STEREO (b): ReadParams::side was set and the file is stereo: the side signal S is at ReadParams::pcm + even(pcm_frames), pcm_frames long (the mid is at pcm, as in every mono load).
  bool pcm_side = false;
};

// Which part of a project a file holds, so the app knows how to use the strategy: Project is a whole project in
// one file (the native format); the others are the files of an OG project folder, each read into the project
// the first one (ProjectSettings) reset.
// Sample is an audio file that makes a sample instrument (.wav, OS-002c): its PCM goes to ReadParams::pcm, the rest into
// the instrument `params.index`.
// Settings is the device settings file (OS-002d, docs/formats/o3-settings.md): no project is involved, the block it reads into
// (writes from) is ReadParams::settings (WriteParams::settings).
enum class FormatKind : std::uint8_t { Project, ProjectSettings, Pattern, Instrument, PatternNames, Sample, Settings };

// Context for one file. `index` is the pattern id (1..255) or the instrument slot (0..) the file belongs to,
// which the OG formats take from the file name; formats that hold a whole project ignore it.
inline constexpr std::uint16_t kWtAuto = 0;
inline constexpr std::uint16_t kWtDetect = 1;
inline constexpr std::uint16_t kWtOff = 0xFFFF;

struct ReadParams {
  std::uint16_t index = 0;
  // The size of the file when the platform knows it, else 0. A reader never needs it to be correct; a .pti
  // reader uses it to skip the PCM instead of reading it through.
  std::uint32_t size_hint = 0;
  // Sample formats (OS-002c). `name` is the file's name (the instrument is named after it, extension cut), `pcm` the room
  // the audio goes to (the sample arena slot: `pcm_capacity` int16 values, word aligned) and `keep_stereo` keeps a stereo
  // file stereo (interleaved; the engine voices play mono until OS-005d, so the default downmixes to (L + R) / 2). A
  // sample reader without `pcm` is refused.
  const char* name = nullptr;
  std::int16_t* pcm = nullptr;
  std::size_t pcm_capacity = 0;
  bool keep_stereo = false;
  // OS-LOADCH: how a stereo file becomes the mono sample (ignored when it is mono or keep_stereo is set): 0 the mean (L + R) / 2, 1 the left channel, 2 the right. Read at the conversion:
  // it needs no more room than the mix does.
  std::uint8_t channel = 0;
  // OS-STEREO (b), the WAV reader only: a stereo file also yields its side signal S = L - M after the mid (wav::Importer::begin `side`): `pcm_capacity` must hold 2 x even(frames) or the read fails LimitExceeded,
  // FormatReport::pcm_side says S is there. `channel` is ignored with it. Other readers ignore it (pcm_side stays false).
  bool side = false;
  // OS-GRAN, WAV and .wt: whether the file is a wavetable (formats/wav_sample.h, WAVETABLE FRAMES). kWtAuto (0): a file that says it is (a Serum 'clm ' chunk, a Surge .wt) is, any other WAV is
  // a sample. kWtDetect (1): as a wavetable whatever the file says, the frame size by its marker or else by its length (the largest of 2048, 1024, 256 that divides it). 32..2048, a power of two:
  // that frame size (over the marker's). kWtOff: never (a marked WAV loads as a plain sample).
  std::uint16_t wavetable = 0;
  // The device settings file (OS-002d): the block (kind Device) the file is read into, which is the defaults again after any error.
  // A reader of that format without it is refused. The arena is not touched.
  model::Settings* settings = nullptr;
  // MED import (OS-008b): the file as a random-access source. An MMD module is a graph of file offsets, which the forward-only
  // `source` of begin() cannot follow; the MED reader needs this and is refused (Internal) without it.
  med::RandomSource* random = nullptr;
  // OS-INSTUI S7, an OG instrument file (.pti, .mti) read ON ITS OWN: the instrument is decoded and checked but not put into the arena's project (InstrumentReader::instrument() has it), and
  // `index` may be any slot 0..63 (it only names the sample path the instrument gets). The caller applies it through the Editor, so that the load is an undo unit. Ignored by every other reader.
  bool standalone = false;
};

struct WriteParams {
  std::uint16_t index = 0;           // pattern id or instrument slot, as in ReadParams
  // .pti: the mono 16-bit little-endian PCM that follows the header, `pcm_frames` frames of it (the platform
  // opens the WAV, skips its header and hands the data over). nullptr writes the header alone.
  core::ByteSource* pcm = nullptr;
  std::uint32_t pcm_frames = 0;
  // The device settings file (OS-002d): the block (kind Device) to write; the `project` of begin() is not used.
  const model::Settings* settings = nullptr;
  // Formats that can be written for more than one generation of the thing that reads them (Export -> OG: official 1.9.0 or
  // 1.9.2): which one, as that format's header names the values. 0 is the default. A writer that has the choice refuses a value
  // it does not know (Malformed); every other writer ignores it.
  std::uint8_t target = 0;
  // OS-INSTUI S7, a .pti written ON ITS OWN (the instrument library, not an Export -> OG): the sample instrument in slot `index`, 0..63, whatever the OG's slots are. The bytes are the same
  // (a .pti carries no slot). Ignored by every other writer.
  bool standalone = false;
  // OS-BUG-PTIRATE: an instrument level 0..100 written in the volume byte in place of the model's, or -1 for the model's. The .pti and .mti volume byte is a level of the 1.2+ law (this OS's Og12 model, smoothing.h);
  // the caller that holds a project played through the 1.1.14 law converts the level (app/pti_load.h pti_check) and hands the converted one here. Only the instrument writer reads it.
  std::int16_t level_override = -1;
  // OS-FILES-C C1b: the fine tune (-100..100 hundredths of a semitone) written in place of the model's, or kNoFineOverride for the model's. The standalone .pti save of an Amiga-engine instrument writes the
  // fine tune the Amiga voice plays (a MED step, app/og_pcm.h og_amiga_fine). Only the instrument writer reads it.
  static constexpr std::int16_t kNoFineOverride = 32767;
  std::int16_t fine_override = kNoFineOverride;
};

class FormatReader {
 public:
  // Starts reading `source` into `arena`. Formats that hold a whole project, and the OG project settings file,
  // reset the arena's project first; the other OG files add to it. A failed read leaves the project as it was
  // before the file (whole-project formats: reset, valid and empty).
  virtual void begin(core::ByteSource& source, model::ModelArena& arena, const ReadParams& params) noexcept = 0;
  // Moves at most about `byte_budget` bytes. Running: call again (on a later pass of the main loop).
  virtual Progress step(std::size_t byte_budget) noexcept = 0;
  virtual Progress progress() const noexcept = 0;
  virtual const FormatReport& report() const noexcept = 0;
  // 0..1000, how much of the file is done when the reader knows (0 when it does not: a job then goes by the bytes read and
  // ReadParams::size_hint).
  virtual unsigned permille() const noexcept { return 0; }
  // Only a reader that changes nothing in the model before it is done (the WAV reader) can be abandoned: cancel() then ends
  // the run as Failed / Cancelled and the project is as it was. The others say false and a job that wraps them is not
  // cancellable (an abandoned pattern or project would be half applied).
  virtual bool can_cancel() const noexcept { return false; }
  virtual void cancel() noexcept {}

 protected:
  FormatReader() = default;
  ~FormatReader() = default;  // not deleted through the interface (no heap, so no virtual destructor slot)
  FormatReader(const FormatReader&) = delete;
  FormatReader& operator=(const FormatReader&) = delete;
};

class FormatWriter {
 public:
  // The project must not change between begin() and Done: step() fails with Modified if it does.
  virtual void begin(const model::Project& project, core::ByteSink& sink, const WriteParams& params) noexcept = 0;
  virtual Progress step(std::size_t byte_budget) noexcept = 0;
  virtual Progress progress() const noexcept = 0;
  virtual const FormatReport& report() const noexcept = 0;

 protected:
  FormatWriter() = default;
  ~FormatWriter() = default;
  FormatWriter(const FormatWriter&) = delete;
  FormatWriter& operator=(const FormatWriter&) = delete;
};

// `head` holds the first `n` bytes of the file (the registry offers up to kSniffBytes), `file_size` its size
// or 0 when unknown. True when they look like this format; never reads past `n`.
// kSniffBytes is 1,084 since OS-MODIMP: a ProTracker module's signature is at offset 1,080. The formats that start with a magic of their own still decide on the first bytes; a caller
// that hands over fewer than 1,084 (or 48 for an S3M, whose signature is at 44) is not offered those two formats by sniff() or identify().
using SniffFn = bool (*)(const std::uint8_t* head, std::size_t n, std::uint32_t file_size) noexcept;
inline constexpr std::size_t kSniffBytes = 1084;

// The name of bit `index` of FormatReport::dropped_groups, "" past the last.
using GroupNameFn = const char* (*)(unsigned index) noexcept;

struct FormatDesc {
  const char* id = "";        // unique, lower case: "native", "og-pattern"...
  const char* title = "";     // for a menu
  // File names this format owns, separated by spaces, compared without regard to case: ".mtp" is an extension
  // (after the last '.' of the name), a word without a leading '.' is a whole file name ("patternsmetadata").
  const char* names = "";
  FormatKind kind = FormatKind::Project;
  SniffFn sniff = nullptr;
  GroupNameFn group_name = nullptr;  // nullptr: the format drops nothing it can name
  FormatReader* reader = nullptr;  // nullptr: this format is only written
  FormatWriter* writer = nullptr;  // nullptr: this format is only read
};

class FormatRegistry {
 public:
  static constexpr std::size_t kMaxFormats = 16;

  // False (nothing added) when the table is full, the id is empty or already registered, or there is neither a
  // reader nor a writer. The descriptor is copied; the strategy objects it points to must outlive the registry.
  bool add(const FormatDesc& desc) noexcept;
  std::size_t count() const noexcept { return count_; }
  const FormatDesc* at(std::size_t index) const noexcept { return index < count_ ? &entries_[index] : nullptr; }

  const FormatDesc* find_id(const char* id) const noexcept;
  // By the extension or the whole name of `file_name`, which may carry a path ('/' or '\\').
  const FormatDesc* find_name(const char* file_name) const noexcept;
  // The first registered format whose sniff function accepts the bytes.
  const FormatDesc* sniff(const std::uint8_t* head, std::size_t n, std::uint32_t file_size) const noexcept;
  // The name decides when it is known, and then the bytes (when given, n > 0) must agree: a file called .mtp that
  // does not look like a pattern is nullptr, not a guess. A name nobody owns falls back to sniffing.
  const FormatDesc* identify(const char* file_name, const std::uint8_t* head, std::size_t n,
                             std::uint32_t file_size) const noexcept;

 private:
  FormatDesc entries_[kMaxFormats];
  std::size_t count_ = 0;
};

}  // namespace o3os::formats
