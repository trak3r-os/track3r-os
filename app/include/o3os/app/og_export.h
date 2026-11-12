// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A whole project in the OG format: the plan and its files (OS-FILES part C, slices C2a and C2b, docs/specs/OS-FILES-C.md section 6). The orchestration is SaveService's (it owns the card's one
// save job: start_og_export); this file is its pure part, testable with no card:
//
//   1. THE FOLDER: Export/<name> OG, then <name> OG 2 .. <name> OG 99, never over an existing one (og_folder_name; the card guard's og_export_folder is the one rule).
//   2. THE PLAN (OgExportPlan): which files, in which order (instruments/NN name.pti, patterns 1..255, patterns/patternsMetadata, project.mt LAST), and the snapshot: the document's revision and the
//      sample bank's version of every sample slot when the export began, rechecked before every file. The PCM stage's decisions and counters (og_instrument_plan) are made once, at the start. An OG folder without project.mt is not a project (the OS's loader says NO PROJECT FILE, the File list probes for it), so a cut at any point leaves a folder
//      that can never be taken for a finished export.
//   3. THE FILE (OgFileSource): a SaveSource that produces one of the three files with the OG writers of formats/og (ProjectWriter v16, PatternWriter v5, PamdWriter v1: the formats of 1.9.0, target 0 only).
//      An instrument's .pti is the standalone .pti save's source (PtiSaveSource: InstrumentWriter v9 with the PCM stage of og_pcm.h), SaveService's.
//
// No allocation, no card access, no format change: every byte comes from the OG writers, which are held to the fixtures byte for byte.

#include <o3os/app/atomic_save.h>
#include <o3os/app/og_survey.h>
#include <o3os/app/pti_load.h>
#include <o3os/formats/og.h>
#include <o3os/hal/save_guard.h>
#include <o3os/model/lane_store.h>
#include <o3os/model/project.h>

#include <cstddef>
#include <cstdint>

namespace o3os::app {

class SampleBank;

// "<project> OG" (n = 1) or "<project> OG <n>" (n 2..99) into out[0..cap): <project> is cut to hal::storage::save::kOgNameRoom bytes (so that "<project> OG 99" fits the guard's 31), loses a trailing
// space or dot, and is "Untitled" when nothing is left. False when n is out of range, `cap` is too small or the result is not a folder name the guard takes.
bool og_folder_name(char* out, std::size_t cap, const char* project, unsigned n) noexcept;

// "pattern_NN.mtp" (NN = id, two digits at least) into out[0..cap); the length, 0 when id is not 1..255 or cap < 19.
std::size_t og_pattern_file_name(char* out, std::size_t cap, unsigned id) noexcept;

// One file of the export.
struct OgFile {
  enum class Kind : std::uint8_t { Pattern, Pamd, Project, Instrument };
  Kind kind = Kind::Project;
  std::uint16_t id = 0;       // Pattern: the pattern id, 1..255; Instrument: the slot, 0..47
  bool in_patterns = false;   // the file goes in the folder's patterns/
  bool in_instruments = false;  // the file goes in the folder's instruments/
  char name[40] = {};         // the file name: pattern_NN.mtp, patternsMetadata, project.mt, NN <stem>.pti (at most 35 bytes)
  char label[16] = {};        // for the strip: PATTERN 07, PATTERN NAMES, PROJECT, INSTRUMENT 03
};

// What the PCM stage does with the sample instrument in `slot` (0..47) of `project`, whose audio `bank` holds (null: no audio anywhere). `count`, when given, takes the counters of the decision
// (formats::og::ExportDrops: resampled, rate_ignored, amiga_engine, bit_engine, pcm_too_long, instruments_not_written, levels_converted, levels_clamped). The decision is a function of the document and the
// bank, so the plan (which counts) and the file (which writes) get the same one.
//   Write    a .pti: `plan` (the PCM resampled, or as it is when identity), `level` (the volume byte to write, -1: the model's: the 1.1.14 level converted, or clamped to 1 when it has no 1.2+ equal),
//            `fine` (the Amiga voice's fine tune, else kNoFineOverride);
//   NoAudio  the slot has no audio: no file (a .pti without PCM is no instrument for the OG loader either);
//   Refused  what a .pti cannot say (pti_load.h PtiVerdict: the Amiga engine in a mode Paula does not play, the 12 / 8 BIT engines, resampled audio longer than a .pti holds): no file, counted.
struct OgInstrumentPlan {
  enum class Result : std::uint8_t { Write, NoAudio, Refused };
  Result result = Result::NoAudio;
  OgPcmPlan plan;
  std::int16_t level = -1;
  std::int16_t fine = formats::WriteParams::kNoFineOverride;
  const std::int16_t* pcm = nullptr;
  std::uint32_t frames = 0;
};
OgInstrumentPlan og_instrument_plan(const model::Project& project, const SampleBank* bank, unsigned slot, formats::og::ExportDrops* count) noexcept;

// The order of the files and the snapshot they are all made from: the instruments' files, the patterns, patternsMetadata, then project.mt.
class OgExportPlan {
 public:
  // Starts a plan over `project` and the audio in `bank` (null: none): decides each sample instrument (og_instrument_plan), counts the files and takes the revision. False when there is nothing to
  // write (a project without patterns).
  // `lanes` (OS-MIDIREC-P1 S7d): the document's MIDI take lanes, only to count their events as lane_events (the .mtp has none); nullptr or a store of another epoch: none.
  bool begin(const model::Project& project, const SampleBank* bank, const model::LaneStore* lanes = nullptr) noexcept;
  // The document or the audio has changed since begin() (any accepted mutation: an edit, an undo, a MIDI take, a sample commit or edit of any slot).
  bool changed() const noexcept;
  // The next file; false when the plan is over. Does not move: advance() does.
  bool next(OgFile& out) const noexcept;
  void advance() noexcept;
  // project.mt is the next (and last) file.
  bool last() const noexcept { return stage_ == Stage::Project; }
  unsigned total() const noexcept { return total_; }
  unsigned taken() const noexcept { return taken_; }
  // About how many bytes the files come to (an upper bound for the pamd file; the instruments' PCM as written): the survey's figure, not a promise.
  std::uint32_t bytes() const noexcept { return bytes_; }
  const model::Project* project() const noexcept { return project_; }
  std::uint32_t generation() const noexcept { return generation_; }  // the snapshot
  // The counters of the PCM stage (and midi_settings is the project writer's), known from the start: what the instruments' files will say.
  const formats::og::ExportDrops& counters() const noexcept { return drops_; }
  // OS-FILES-C C2c, the question before an export: every counter the export will end with, the writers' included (tracks 13-16, FX, modules, pan / solo / bus, names cut...), got by running each writer
  // over the plan's snapshot into a sink that counts and keeps nothing (the project file, every pattern, the names file, the head of every instrument's file). The same totals as the files will add up to
  // (a test holds the two equal). Sliced (the minors of the C2c review): survey_start() puts `out` at the plan's counters, then each survey_step() runs ONE writer (project.mt, patternsMetadata, one pattern
  // or one instrument head: about the cost of building that file's bytes, 4 KB of stack) and adds its counters; the step that finds nothing left returns true. survey() is the whole run at once (the tests').
  void survey_start(formats::og::ExportDrops& out) noexcept;
  bool survey_step(formats::og::ExportDrops& out) noexcept;
  void survey(formats::og::ExportDrops& out) noexcept;

 private:
  enum class Stage : std::uint8_t { Instruments, Patterns, Pamd, Project, Done };
  enum class SurveyStage : std::uint8_t { Project, Pamd, Patterns, Instruments, Done };

  void seek_instrument() noexcept;
  void seek_pattern() noexcept;

  const model::Project* project_ = nullptr;
  const SampleBank* bank_ = nullptr;
  std::uint32_t generation_ = 0;
  std::uint32_t versions_[hal::storage::save::kOgFolderLastSlot] = {};  // the bank's version of each of the 48 sample slots when the plan began
  std::uint64_t write_mask_ = 0;  // bit n: slot n gets a .pti
  Stage stage_ = Stage::Done;
  unsigned slot_ = 0;     // the next instrument slot to look at
  unsigned cursor_ = 1;   // the next pattern id to look at
  formats::og::ExportDrops drops_;
  SurveyStage survey_stage_ = SurveyStage::Done;  // the sliced survey's cursor: the writer the next survey_step() runs, and the pattern id or slot it has reached
  unsigned survey_next_ = 0;
  unsigned total_ = 0;
  unsigned taken_ = 0;
  std::uint32_t bytes_ = 0;
};

// The source of one file: the OG writer of its kind over the project, stepped a budget at a time. The three writers share a class so that the service holds one object.
class OgFileSource final : public SaveSource {
 public:
  // Arms the source for `file` of `project`, which must still be at `generation` (the plan's snapshot) when the writer starts at begin() and at every step: the job reaches begin() a few passes after
  // the plan started it, and a change in between would otherwise slip into a file of another revision than the files before it.
  void set(const model::Project& project, const OgFile& file, std::uint32_t generation) noexcept {
    project_ = &project;
    kind_ = file.kind;
    id_ = file.id;
    generation_ = generation;
  }
  void begin(core::ByteSink& sink) noexcept override;
  formats::Progress step(std::size_t byte_budget) noexcept override;
  const char* failure() const noexcept override { return failure_; }
  std::uint32_t size_hint() const noexcept override;
  // What the file left out or rewrote (valid once its job is done).
  const formats::og::ExportDrops& drops() const noexcept;

 private:
  const model::Project* project_ = nullptr;
  OgFile::Kind kind_ = OgFile::Kind::Project;
  std::uint16_t id_ = 0;
  std::uint32_t generation_ = 0;
  const char* failure_ = "";
  formats::og::ProjectWriter project_writer_;
  formats::og::PatternWriter pattern_writer_;
  formats::og::PamdWriter pamd_writer_;
};

}  // namespace o3os::app
