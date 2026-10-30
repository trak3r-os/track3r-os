// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Loads the sample PCM of a project that app::OgFolderLoader has read (OS-007f): every sample instrument of the document gets its audio from the card into
// the sample arena (SDRAM) and is bound to the voice pool's table, one slot a slot. Where the OG keeps a sample (docs/formats/og.md section 5):
//
//   instruments/instrument_NN.pti, NN name.pti   392 bytes of head, then the PCM: mono signed 16-bit little-endian, 44.1 kHz. A slot a .pti was applied to
//                                                (OgFolderLoader::pti_slots()) takes its audio from there, read straight into the arena in chunks.
//   samples/instrNN.wav  (the model's sample_path)  the 1.1 pair of an .mti, and the copy that sits beside a .pti in an official 1.9 Workspace. The Workspace WAV
//                                                of an official 1.9 project is 16-bit mono (observed in the fixture); whether the firmware converts again when it loads is an open research
//                                                question. This loader goes through the WAV importer of formats/wav, which reads 8/16/24-bit and float, mono or stereo (see "OUR CHOICES").
//
// WHAT IS BOUND (make_sample_desc): the instrument's start and end (0..65535 of the sample, converted by floor(point * length / 65535): our conversion, see og_samples.cpp) cut the window the
// voices see, a forward loop (play mode 1) becomes the voices' loop over [loop1, loop2) inside it, volume 0..100 becomes the instrument's Q16 gain through the
// project's law (instrument_law below; OS-007i), tune and fine tune go to the voices (voices.h tuned_inc_q32). The play modes that are not "a window at the note's pitch" are bound as this engine plays them
// (OS-007h: an official 1.9 project is mostly these, and played as plain windows it came out three octaves low and garbled):
//   slice (4)       the SELECTED slice is the window ([slices[sel], slices[sel + 1]), the last slice to the end of the sample), played at the note's pitch;
//   beat slice (5)  the whole sample with a table of the slices' boundaries (slice_table_samples() 16-bit samples, kept in the arena after the PCM): the
//                   voices pick the slice from the note (note - 24) and play it at the root's pitch;
//   wavetable (6)   the instrument's current window (of wavetable_window_size frames, 2048 unless it names a valid size) as a loop, with the wavetable pitch law.
// Backward and ping-pong loops (2, 3; OS-007k) are the voices' own loop directions, with the loop conditions of mode 1 (a loop that fails them is a one shot).
// Granular (7; OS-GRAN) is bound as this engine plays it: the WHOLE sample (start, end and loop points are not used), the grain's length, position and shape, and its type (forward,
// backward, ping-pong) as the descriptor's loop mode (audio/voices.h GRANULAR).
// OS-007k also pushes the instrument's pan (0..100, the voices hand it to the note's track) and its amp envelope (slot 0: delay, attack, hold, decay, sustain,
// release in milliseconds, amount), converted to the engine's units here; an amp slot that is an LFO (its loop flag set) keeps the plain gain (TODO). The FX Slice (a slice
// chosen by a step), the other envelopes, the LFOs, sends and filter are not pushed (005d-lite).
//
// WHAT HAPPENS TO WHAT IS NOT THERE: an instrument whose file is absent, unreadable or not usable stays silent and the load goes on (OS-007f: the load
// never fails; `problems` keeps the first few reasons). Not usable: not 44,100 Hz (the rate an OG folder holds; whether the official firmware refuses other rates is an open research question), no audio, a window with start >= end. An
// instrument that does not fit what is left of the arena stays silent (`no_room`) and the next one is tried: a smaller one may still fit, and the importer
// and the file size say so before a byte is read. A card error ends the loading (`storage_error`; the instruments that had not had their turn are counted
// in `missing`). Arena order: the .pti slots in the order of their directory listing, then the other sample instruments by slot.
//
// OUR CHOICES for files that only a hand-copied card holds (the Workspace WAVs of an official project are 16-bit mono): a stereo WAV is
// mixed (L + R) / 2 by the importer, and 24-bit and float are dithered. A second .pti for
// a slot some .pti was applied to: the first one listed supplies the PCM.
//
// READ ONLY, as the storage contract: open, read, seek, size, close, opendir, readdir, closedir.
// Contexts: main loop (never the audio interrupt). The voice table is written with set_sample(), which is the main loop's call; the caller has to have
// silenced the voices and cleared the table first (a slot is never rewritten under a playing voice, voices.h). One step() moves about `byte_budget` bytes
// and does at most kMaxActionsPerStep directory entries or file opens. No heap; the importer (1.1 KB) is a member, so the loader is a static of its owner.

#include <o3os/app/og_folder.h>
#include <o3os/app/sample_bank.h>
#include <o3os/audio/smoothing.h>
#include <o3os/audio/voices.h>
#include <o3os/formats/wav.h>
#include <o3os/hal/storage.h>
#include <o3os/model/instrument.h>
#include <o3os/model/project.h>

#include <cstddef>
#include <cstdint>

namespace o3os::app {

enum class SampleProblem : std::uint8_t {
  NotFound,    // no file (or the instrument names none)
  Unreadable,  // the file would not open or read, or is shorter than its size said, or the importer refused it
  Unusable,    // a file this loader refuses (not 44,100 Hz, no audio) or points that leave no sound (start >= end)
  NoRoom,      // the arena was full
};
const char* sample_problem_name(SampleProblem p) noexcept;

struct SampleLoadReport {
  struct Problem {
    std::uint8_t slot = 0;
    SampleProblem why = SampleProblem::NotFound;
  };
  static constexpr unsigned kMaxProblems = 8;

  std::uint32_t loaded = 0;        // sample instruments bound to a voice slot
  std::uint32_t missing = 0;       // silent: NotFound, Unreadable or Unusable (also the ones a card error left unread)
  std::uint32_t no_room = 0;       // silent: the arena was full (the one that did not fit and the ones after it)
  std::uint32_t approximated = 0;  // loaded, but their play mode is one the voices do not have yet: played as one shot (none since OS-GRAN: a beat slice whose table had no room)
  std::uint32_t samples = 0;       // int16 samples placed in the arena (each sample starts on an even index)
  std::uint32_t stereo_lost = 0;   // OS-STEREO (b): stereo instruments (flag set) whose side file was missing, unreadable or did not match: loaded mono, the flag cleared
  hal::storage::Status storage_error = hal::storage::Status::Ok;  // a card error that ended the load
  Problem problems[kMaxProblems];  // the first problems, in the order they were found
  std::uint8_t problem_count = 0;  // how many of them are filled in (at most kMaxProblems)
};

// The 16-bit samples of arena a beat slice instrument's slice table needs after its PCM (two halves a boundary, slice_count + 1 boundaries); 0 for every
// other instrument (and for a beat slice with no slices: the whole sample is its one slice).
std::uint32_t slice_table_samples(const model::Instrument& in) noexcept;

// The law an instrument's volume 0..100 goes through (smoothing.h, THE LEVEL LAWS; OS-007i), by the structure version the OG project file reports
// (OgLoadReport::version): the 1.2+ law (-24..+24 dB, 50 is 0 dB) from version 7, the first the 1.2.0 firmware writes (the v1.2.0 fixture; PAR-006 section 3.1),
// the 1.1.14 table below it (a file version of 6 or less; 0, no project read, is below it too). The model has no field for the origin
// of an instrument or a project, so the importer's version is the evidence, passed to begin(). It is the version of the FILE, not of the firmware that plays
// it: whether a 1.2+ firmware converts the volumes of an older project when it opens it is not known (an open research question). Both laws are this OS's model; the file version only picks one.
constexpr audio::LevelLaw instrument_law(std::uint8_t project_version) noexcept {
  return project_version >= 7u ? audio::LevelLaw::Og12 : audio::LevelLaw::Og1114;
}

// OS-BUG-PASTE24: the law is the DOCUMENT's (model::Project::volume_law, runtime state), not a copy held by each user of it. The OG loader commits it when a load has succeeded; a native project, an import and
// a new project are the 1.2+ law (Project::reset). The sample bank reads it at every rebuild.
inline audio::LevelLaw project_law(const model::Project& project) noexcept {
  return project.volume_law() != 0u ? audio::LevelLaw::Og1114 : audio::LevelLaw::Og12;
}
// Commits the law of a successfully loaded OG project of structure `version` into the document (instrument_law above).
inline void commit_og_law(model::Project& project, std::uint8_t version) noexcept {
  project.set_volume_law(instrument_law(version) == audio::LevelLaw::Og1114 ? 1u : 0u);
}
// The platform's rule for a load of an OG project (begin_samples is where its Done is acted on): the law is committed only when the load has succeeded, so a load that fails (or a folder that is
// not a project) leaves the law as it was. (That holds for a failure before the project reader resets the document, a folder that is no project among them; a project.mt that fails to parse fails after
// that reset, which leaves an empty document and the 1.2+ law: nothing is left to read through the old one.)
inline bool commit_og_law_if_done(model::Project& project, const OgFolderLoader& loader) noexcept {
  if (loader.state() != OgFolderLoader::State::Done) {
    return false;
  }
  commit_og_law(project, loader.report().version);
  return true;
}
// The level that is 0 dB under `law` (the demo kit is bound at unity).
constexpr std::uint8_t unity_level(audio::LevelLaw law) noexcept { return law == audio::LevelLaw::Og1114 ? std::uint8_t{100} : std::uint8_t{50}; }

// What the voices get for `in` over `frames` samples of PCM at `pcm` (mono, in the arena). length 0 when the window is empty (start >= end, a wavetable of
// less than one window, or no PCM): the slot then stays silent. `approximated` (when not null) is set for a beat slice whose table was not given. `slice_table` (when not null) is where slice_table_samples(in) samples are written, 2-byte aligned
// and at an even index of the arena (4-byte aligned) when it is the arena; a beat slice instrument is bound to it. `law` turns the instrument's volume into
// SampleDesc::gain (main loop: calls level_gain_q16).
audio::SampleDesc make_sample_desc(const model::Instrument& in, const std::int16_t* pcm, std::uint32_t frames, bool* approximated, std::int16_t* slice_table = nullptr,
                                   audio::LevelLaw law = audio::LevelLaw::Og12) noexcept;

// Does make_sample_desc read instrument parameter `field` (model::instrument_param): the voice, filter and envelope fields it turns into the descriptor's gain, pan, tune, filter
// and modulators (OS-LIVEP; OS-MOD: the amp, pan, cutoff and finetune slots; OS-GRAN: the wavetable and granular position slots). An edit of one must reach the engine's descriptor of the slot
// (SampleBank::request_rebind); OS-SENDFX and OS-IFX1 added the sends, the glide, the overdrive and the bit depth.
bool sample_desc_reads(unsigned field) noexcept;

// OS-MOD, LIVE LOOP (audio/voices.h): the ParamChanges (kind Instrument, field audio::inst_param::LoopStart / LoopEnd, value in samples from the descriptor's data) that move the loop of a SOUNDING
// instrument from `before` to `after`, in the order that keeps each one valid (the render holds a point to the other, 2 samples apart). Only when both bind the same loop kind over the
// same window (the same start, end and play mode: the PCM and the window are the handover's, not the ring's) and both have a loop; otherwise 0, and the handover rebinds when playback has
// stopped. `frames` is the whole sample's. Pure. Returns how many of `out` (2) are set.
struct LoopMove {
  unsigned field;
  float value;
};
unsigned live_loop_moves(const model::Instrument& before, const model::Instrument& after, std::uint32_t frames, LoopMove out[2]) noexcept;

// One line for `diag inst`: what is bound for slot `slot` (`in` and `d` are the document's instrument and the voices' descriptor of it; `in` null or not a sample
// instrument, or `d` empty, say so): "inst 03: mode 0 start 0 len 4096 loop 0..0 root 60 rate 44100 vol 50 gain +0.0 dB sum 1a2b", plus " beat slices N first-note 24" for
// a beat slice, or " wt W" for a wavetable. start and len are frames of the window the voices play, counted from the start of the instrument's PCM; root is the
// note that plays the sample at its own pitch; vol is the stored level 0..100 and gain what it became (dB to a tenth, "-inf dB" for a silent one); the checksum is the 16-bit sum of the first 64 samples of the window (unsigned halfwords), for the bench to set
// against the file. An instrument with nothing bound reads "inst 05: mode 0, silent: nothing is bound vol 50 gain +0.0 dB" (OS-BUG-PASTE24 E2): the same " vol N gain X dB" words, the gain being what the
// stored level makes through `law` when one is passed (no descriptor holds one yet), else the descriptor's. Never longer than `cap`, always terminated; returns its length.
std::size_t describe_instrument(unsigned slot, const model::Instrument* in, const audio::SampleDesc& d, char* out, std::size_t cap, int law = -1, bool demo = false, int bank_law = -1, int voices_law = -1) noexcept;

// OS-FILES: PCM that is already decoded and is not on the card as a WAV: the samples of a module import (app/module_import.h). The loader asks for each sample instrument's frames and
// rate, then for the frames in pieces. Main loop only; a read that takes long is the feed's to bound (a piece is at most OgSampleLoader::kFeedChunkFrames).
class PcmFeed {
 public:
  virtual std::uint32_t frames(unsigned slot) const noexcept = 0;  // mono 16-bit frames of instrument `slot`; 0: it has none
  virtual std::uint32_t rate(unsigned slot) const noexcept = 0;    // the Hz the frames are meant to be played at
  virtual bool read(unsigned slot, std::uint32_t first, std::uint32_t n, std::int16_t* dst) noexcept = 0;

 protected:
  PcmFeed() = default;
  ~PcmFeed() = default;
};

class OgSampleLoader {
 public:
  enum class State : std::uint8_t { Idle, Running, Done };

  static constexpr unsigned kMaxActionsPerStep = 32;               // directory entries or file opens a step may do
  static constexpr std::uint32_t kMaxEntriesPerDirectory = 4096;   // as OgFolderLoader: a bound nobody reaches
  static constexpr std::uint32_t kPtiHeadBytes = 392;              // formats::og::kInstrumentBytes: the PCM of a .pti starts here
  static constexpr std::uint32_t kPtiChunkBytes = 16 * 1024;       // the most one read of a .pti's PCM asks for
  static constexpr std::uint32_t kFeedChunkFrames = 4096;          // the most one read of a PcmFeed asks for

  // Starts loading the PCM of `project`'s sample instruments from `folder` (the one OgFolderLoader::folder() names: UTF-8, no leading or trailing '/') of
  // `storage` into `arena` (`arena_samples` int16 values, 4-byte aligned), binding each to `voices`. `pti_slots` is OgFolderLoader::pti_slots(). False
  // when the folder is not a valid path or does not fit (nothing started). Anything running is abandoned. Everything passed must outlive the load.
  // `law` is the instrument volume law of the project: instrument_law(OgFolderLoader::report().version).
  // OS-011a: with a `bank` the load starts it over (clear) and tells it where each slot's whole sample went and how far the arena is used, so that the sample
  // screens can work on what a project load placed.
  // OS-RATEPITCH: `native` (a folder this OS saved: NativeFolderLoader) takes a WAV of the rate the instrument stores (INST sample_rate; the save wrote it so) as it is: the descriptor
  // plays it at that rate's pitch. An OG folder takes 44,100 Hz only (whether the official firmware accepts other rates is an open research question). A native WAV of another rate than the instrument's is unusable.
  bool begin(hal::storage::Storage& storage, const char* folder, const model::Project& project, std::uint64_t pti_slots, audio::VoicePool& voices,
             std::int16_t* arena, std::uint32_t arena_samples, audio::LevelLaw law = audio::LevelLaw::Og12, SampleBank* bank = nullptr, bool native = false) noexcept;
  // OS-FILES: the same load for a PcmFeed instead of a folder: every sample instrument of `project` that the feed has frames for gets them, in slot order, into the arena and
  // bound to `voices` with the feed's rate; the bank is started over, and nothing is marked as on the card (the samples belong to no folder until a save writes them). An instrument the
  // feed has no frames for stays silent (`missing`); one that does not fit stays silent too (`no_room`). Same states, report and step() as begin().
  bool begin_feed(PcmFeed& feed, const model::Project& project, audio::VoicePool& voices, std::int16_t* arena, std::uint32_t arena_samples, audio::LevelLaw law = audio::LevelLaw::Og12,
                  SampleBank* bank = nullptr) noexcept;
  // A slice of the work. Returns the state after it: Running (call again) or Done (always: nothing here fails the load).
  State step(std::size_t byte_budget) noexcept;
  // Abandons a load and closes its files; what was bound stays bound.
  void abort() noexcept;

  State state() const noexcept { return state_; }
  const SampleLoadReport& report() const noexcept { return report_; }

 private:
  enum class Stage : std::uint8_t { ListOpen, ListNext, PtiRead, WavNext, WavRead, SideNext, SideRead, Finish, FeedNext, FeedRead };

  void advance(std::size_t room) noexcept;
  void list_open() noexcept;
  void list_next() noexcept;
  void pti_read(std::size_t room) noexcept;
  void wav_next() noexcept;
  void wav_read(std::size_t room) noexcept;
  void feed_next() noexcept;
  void feed_read(std::size_t room) noexcept;
  bool open_file(const char* sub, const char* name, std::uint8_t slot) noexcept;  // path_ = base_/sub/name, open it into file_; false: the slot is settled
  // `rate_hz`: the WAV's rate (0: the instrument's). `side` (OS-STEREO (b)): S follows M in the arena (at used_ + even(frames)) and the slot owns both.
  void bind(std::uint8_t slot, std::uint32_t frames, std::uint32_t rate_hz = 0, bool side = false) noexcept;
  void side_next() noexcept;   // OS-STEREO (b): opens the side file of the instrument whose M was just read; no usable S binds M alone
  void side_read(std::size_t room) noexcept;
  void bind_and_mark(bool side) noexcept;  // bind(slot_, m_frames_, m_rate_, side) and, from a folder, the slot's audio is on the card as it stands
  void settle(std::uint8_t slot, SampleProblem why) noexcept;  // the slot stays silent: counted, remembered, never tried again
  void card_failed(hal::storage::Status s) noexcept;
  void finish() noexcept;
  bool is_sample_slot(unsigned slot) const noexcept;
  bool join(const char* a, const char* b) noexcept;  // path_ = base_ [/ a [/ b]]
  void close_all() noexcept;
  std::uint32_t moved() const noexcept { return source_.total() + pti_bytes_; }

  hal::storage::Storage* storage_ = nullptr;
  const model::Project* project_ = nullptr;
  audio::VoicePool* voices_ = nullptr;
  SampleBank* bank_ = nullptr;
  PcmFeed* feed_ = nullptr;              // OS-FILES: set by begin_feed (null for a folder load)
  std::int16_t* arena_ = nullptr;
  std::uint32_t arena_samples_ = 0;
  std::uint32_t used_ = 0;               // samples of the arena in use (even)
  State state_ = State::Idle;
  Stage stage_ = Stage::Finish;
  audio::LevelLaw law_ = audio::LevelLaw::Og12;  // the instrument volume law of this load
  bool native_ = false;                  // the folder is a native project's: a WAV keeps the rate the instrument stores
  std::uint64_t pti_slots_ = 0;
  std::uint64_t settled_ = 0;            // bit n: slot n is done (bound, or silent for a reason)
  std::uint8_t slot_ = 0;                // the slot being loaded
  std::uint8_t cursor_ = 0;              // WavNext: the next slot to look at
  std::uint32_t entries_ = 0;
  std::uint32_t pti_left_ = 0;           // bytes of PCM of the .pti still to read
  std::uint32_t pti_frames_ = 0;
  std::uint32_t pti_bytes_ = 0;          // bytes read straight into the arena (wraps: only differences are used)
  std::uint32_t m_frames_ = 0;           // OS-STEREO (b): the mid just read, while its side file is read after it
  std::uint32_t m_rate_ = 0;
  hal::storage::FileStamp m_stamp_{};
  hal::storage::FileHandle file_{};
  hal::storage::DirHandle dir_{};
  SampleLoadReport report_{};
  StorageSource source_;
  formats::wav::Importer importer_;
  char base_[OgFolderLoader::kFolderMax + 1] = {};
  char path_[hal::storage::kPathMax + 1] = {};
  hal::storage::DirEntry entry_{};
};

// "samples 18 loaded, 2 missing, 3210 KB" (", 1 no room" and the card's error when there were any) for the log (the caller puts "boot: " in front) and
// `diag sd`. Written into `out` (at most `cap` - 1 characters, always terminated); returns its length.
std::size_t describe_samples(const SampleLoadReport& report, char* out, std::size_t cap) noexcept;
// "load: instrument 07 sample not found": one of the report's problems, for the log. Same rules.
std::size_t describe_problem(const SampleLoadReport::Problem& problem, char* out, std::size_t cap) noexcept;

}  // namespace o3os::app
