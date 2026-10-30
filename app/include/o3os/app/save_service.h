// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The saves of the device (OS-007n): what the File, Record and Sample edit screens ask for and the device settings ask for by themselves, as one object that owns the
// card's one save job (AtomicSave) and what feeds it. It is the only thing above the HAL that holds a hal::storage::StorageWriter; an action reaches it through
// ActionContext::saves and asks for "save this project", "save this audio as a WAV", never for a path (the names are built here, from the guard's own: hal/save_guard.h).
//
//   PROJECT   Projects/<name>/project.o3pj, the native format (formats/native, no format change), written from the attached document. SAVE replaces the file of the project
//             the user is in (the folder it was loaded from or last saved to) and keeps the previous version as project.o3pj.bak; SAVE AS makes a new folder, named on the on-screen keyboard
//             (OS-FU2; it starts with the first free "Track 01", "Track 02" ...) and never replaces anything. A project that was loaded from outside Projects/ (the
//             Workspace folder) is saved as a new one.
//   SAMPLES   (OS-FU2) A project's samples go with it: Projects/<name>/samples/<file>.wav, the file the instrument names (its sample path, "samples/instr01.wav"), 16-bit mono at the
//             instrument's rate, written FROM THE SAMPLE BANK (the audio in memory), one file at a time, each atomically (AtomicSave: temp, read-back, rename; a changed sample replaces
//             its file and keeps the old one as <file>.bak), BEFORE the project file, so that a cut never leaves a project naming a sample the folder does not have. A slot is written
//             unless the folder already holds that audio: it was read from that folder's own samples/ and has not changed since (SampleBank::card_version), or an earlier save wrote
//             exactly that version into it. So an OG project with .pti instruments, whose audio lives in no WAV, gets its WAVs in the native folder (and reopens with its sound), a Save As
//             copies every sample into the new folder, and a plain Save writes only what was edited or loaded since. OS-BUG-OGSAVE: a Save in place of an OG project does not rewrite a WAV that is already there and already holds that audio (the official firmware's, odd data chunk and all): before a replacing sample
//             job writes it reads the file for an audio the folder is not known to hold and ends untouched when it is the same frames in the same format (WavSaveSource as HeldCheck; ADR 0006 amendment 2).
//             A sample that cannot be written (not a name the guard takes, too big
//             for a WAV file) is skipped; one that fails stops the save, the project file is then not written (the strip says why). No format changes: sample_path is an existing field.
//   WAV       Samples/REC_001.wav, SMP_001.wav ...: 16-bit PCM, mono, the audio as the bank or the take holds it, never over an existing file (the next free number).
//   PTI       (OS-INSTUI S7) instruments/<name>.pti: one sample instrument and its audio in the OG's instrument format (formats/og InstrumentWriter, WriteParams::standalone; no new format), made from the
//             instrument's name (sanitised: letters, digits, space, '_', '-', '.', the guard's valid_name; "<name> 2" .. "<name> 99" when the name is taken) and NEVER over an existing file.
//   DELETE    (OS-FILES part B) DELETE marks a project folder by creating the empty file Projects/<name>/.deleted (delete_project): the File list hides a folder that holds it, nothing is moved,
//             renamed or erased (a folder is never renamed: even inside one folder FAT writes two directory entries, which can sit in different sectors: docs/specs/OS-FILES-B.md). PURGE touches
//             the document only. The open project is never marked.
//   RECENT    (OS-FILES part B) Settings/recent.o3rc (app/recent_list.h): the last five projects opened or saved, written by itself after each change like the settings, quietly, and read back when the
//             File screen lists the projects.
//   SETTINGS  Settings/device.o3st (formats/settings), written by itself 1.5 s after the last change of a device setting or Learn binding, quietly: the job's slot is not on
//             the notification strip unless it fails.
//   EXPORT    (OS-WAVEXP) Export/<project>/<project>_Song0001.wav and <project>_P<n>_0001.wav: the MAIN mix of the song or of a pattern, rendered offline, 16-bit stereo at 44,100 Hz
//             (app/export_source.h), never over an existing file (the next free number; the project part of the name is cut so that the whole stem fits the guard's 31). <project> is the
//             folder name of the project the user is in, "Untitled" when there is none. The render and the card are the job's; the job is a normal save, so a cancel, a failure or a pulled card
//             leaves no file under the final name.
//   STEMS     (OS-WAVEXP slice B) a set of such files in Export/<project>/, one job each: <project>_Song_S0001_T01.wav .. _DELAY .. _REVERB .. _MIX (or _P<n>_S0001_), the set number the first
//             that none of a set's names uses. A set is a plan like the project's: it ends with its last file, or with the first that is cancelled or fails; the files already finished stay.
//   OG EXPORT (OS-FILES-C C2a) Export/<project> OG[ n]/: a whole project in the OG format, a set of files like the stems' (one job each, the first that fails or is cancelled ends it, the files finished stay):
//             the sample instruments (C2b: instruments/NN <name>.pti, slots 1-48, the PCM stage of og_pcm.h: resampled to 44,100 Hz, the 1.1.14 levels converted: the standalone .pti save's source),
//             the patterns (patterns/pattern_NN.mtp), patterns/patternsMetadata, then project.mt LAST (a folder without it is no project). Each file is a NewOnly save of the OG writers (formats/og, 1.9.0), never
//             over an existing file, in a folder that did not exist (the first free of OG, OG 2 .. OG 99). The document's revision is taken when it begins and rechecked before every file, with the bank's version of every sample slot: a change ends it
//             ("MODIFIED", no project.mt). The counters of the PCM stage are known when it starts (og_drops). Started by the console (`ogexp`); no key, no screen.
//   SELECTION (OS-WAVEXP slice B) Export/<project>/<project>_SEL0001.wav, one file, never over another.
//   APP FILE  (OS-H4, ADR 0014 amendment A) AppExport/<app>/<stem>.<ext>: a file an SD app module wrote through the host's export calls (app/app_export.h owns the calls and the staging ring; this
//             object starts the job and keeps its result). A normal NewOnly save (Temp, read-back, rename; a taken name becomes <stem>_001 .. _999), never over an existing file. It YIELDS to the
//             user: a save, an export or a DELETE the user asks for while it runs cancels it like the RECENT file (the app is told FAILED, the Temp is removed), so the core's own saves are never
//             refused because of an app; the user's save does not wait for it.
//
// Maintainer-approved policy (2026-10-12): the saver deletes its own old .bak (never anything else) and the settings save themselves 1.5 s after the last change. All of it goes through the gate of hal/storage_write.h (any mounted, writable FAT card is saved to: no marker, OS-ANYCARD). Main loop only. Memory: about 5 KB (the job with its read-back buffer, the native writer, the settings buffer), which the platform places in SDRAM
// (the tail of the sample arena), never in SRAM_U.

#include <o3os/app/app_export.h>
#include <o3os/app/atomic_save.h>
#include <o3os/app/dir_scan.h>
#include <o3os/app/export_source.h>
#include <o3os/app/export_stems.h>
#include <o3os/app/og_export.h>
#include <o3os/app/og_pcm.h>
#include <o3os/app/og_timing.h>
#include <o3os/app/recent_list.h>
#include <o3os/audio/offline.h>
#include <o3os/core/job.h>
#include <o3os/formats/native.h>
#include <o3os/formats/og.h>
#include <o3os/formats/settings.h>
#include <o3os/hal/storage.h>
#include <o3os/hal/storage_write.h>
#include <o3os/model/project.h>
#include <o3os/model/settings.h>

#include <cstddef>
#include <cstring>
#include <cstdint>

namespace o3os::app {

class SampleBank;
class Recorder;

// The source of a project file: the native writer over the project, stepped a budget at a time.
class NativeSaveSource final : public SaveSource {
 public:
  // `stereo_mask`: bit n lets instrument n's stereo flag be written (formats::native::Writer::begin): SaveService holds a flag back where the side file is not saved.
  void set(const model::Project& project, std::uint64_t stereo_mask = ~std::uint64_t{0}) noexcept {
    project_ = &project;
    stereo_mask_ = stereo_mask;
  }
  void begin(core::ByteSink& sink) noexcept override;
  formats::Progress step(std::size_t byte_budget) noexcept override;
  const char* failure() const noexcept override;
  std::uint32_t size_hint() const noexcept override { return writer_.total_bytes(); }

 private:
  const model::Project* project_ = nullptr;
  std::uint64_t stereo_mask_ = ~std::uint64_t{0};
  formats::native::Writer writer_;
};

// The source of a 16-bit PCM WAV: the 44-byte header, then the samples as they are in memory (little-endian). It checks that the audio is still the audio it was asked to
// write before every slice, and fails ("CHANGED WHILE WRITING") when it is not.
// It is also the HeldCheck of a replacing save (OS-BUG-OGSAVE): "does the file already hold this audio?" is true when the file is a PCM WAV of the same channels, rate and 16 bits whose data
// chunk holds the same whole frames, byte for byte (a stray half frame after them, an odd data chunk with no pad byte as the official firmware writes, is ignored), whatever other chunks and
// whichever header the file has before the data (at most kHeldHeaderMax bytes of it). The bytes are compared as they are read, a chunk a step; the audio changing meanwhile (the Check) is a no.
class WavSaveSource final : public SaveSource, public HeldCheck {
 public:
  static constexpr std::size_t kHeaderBytes = 44;
  static constexpr std::uint32_t kHeldHeaderMax = 4096;  // the bytes of a file's header (chunks before its data) that the held check reads before it gives up
  // `check` says whether the audio is still the same (a bank slot's version, a take that is still in review); nullptr: it cannot change.
  using Check = bool (*)(const void* ctx) noexcept;
  bool set(const std::int16_t* pcm, std::uint32_t frames, std::uint8_t channels, std::uint32_t rate, Check check, const void* ctx) noexcept;
  void begin(core::ByteSink& sink) noexcept override;
  formats::Progress step(std::size_t byte_budget) noexcept override;
  const char* failure() const noexcept override { return failure_; }
  std::uint32_t size_hint() const noexcept override { return static_cast<std::uint32_t>(kHeaderBytes + data_bytes_); }
  // The header for `frames` of `channels` at `rate`, into `out` (kHeaderBytes).
  static void make_header(std::uint8_t* out, std::uint32_t frames, std::uint8_t channels, std::uint32_t rate) noexcept;
  // HeldCheck (after set()): the file of `file_size` bytes is to be fed from its first byte.
  bool start(std::uint32_t file_size) noexcept override;
  formats::Progress feed(const std::uint8_t* data, std::size_t n) noexcept override;

 private:
  enum class Held : std::uint8_t { Riff, ChunkHead, Fmt, Skip, Data };
  bool fmt_ok() const noexcept;
  const std::int16_t* pcm_ = nullptr;
  std::uint32_t frames_ = 0;
  std::uint32_t rate_ = 0;
  std::uint8_t channels_ = 1;
  std::uint64_t data_bytes_ = 0;
  std::uint64_t pos_ = 0;  // bytes of the file produced
  Check check_ = nullptr;
  const void* ctx_ = nullptr;
  const char* failure_ = "";
  core::ByteSink* sink_ = nullptr;
  std::uint8_t header_[kHeaderBytes] = {};  // (the held check collects the head of a file's chunks here: it is rebuilt by begin() before a write)
  Held held_ = Held::Riff;
  bool held_fmt_ = false;       // the 'fmt ' chunk was seen and is the format of this audio
  std::uint8_t held_have_ = 0;  // bytes of the head being collected in header_
  std::uint8_t held_need_ = 0;  // ... and how many it takes
  std::uint32_t held_left_ = 0;  // bytes left of the chunk being skipped (its pad byte included), or of the data to compare
  std::uint32_t held_off_ = 0;   // bytes of the file consumed before its data
  std::uint32_t held_pos_ = 0;   // bytes of the audio compared
};

// The source of a .pti (OS-INSTUI S7): the OG instrument writer over one sample instrument of the project and the audio the bank holds for it (a byte source over the samples, little-endian as they are
// in memory). It checks that the audio is still the audio it was asked to write before every slice (CHANGED WHILE SAVING), and the writer itself fails when the project changes.
class PtiSaveSource final : public SaveSource {
 public:
  using Check = WavSaveSource::Check;
  // False when the instrument is not a sample instrument or the audio is empty or longer than a .pti holds.
  // `level`: the volume byte to write when it is not the model's (OS-BUG-PTIRATE, pti_check), else -1.
  bool set(const model::Project& project, unsigned slot, const std::int16_t* pcm, std::uint32_t frames, Check check, const void* ctx, std::int16_t level = -1) noexcept;
  // OS-FILES-C C1b: the audio is resampled by `plan` (not identity) before it is written, with `kernel` (SampleBank::resample_kernel) and the instrument's loop in the audio's own frames; the file then
  // holds plan.frames_out frames. `fine`: the fine tune to write (WriteParams::kNoFineOverride: the model's). Call after set(); the audio, the kernel and the project stay valid and unchanged until the save ends.
  void set_resample(const OgPcmPlan& plan, AmigaKernel& kernel, std::int16_t fine) noexcept;
  const formats::og::ExportDrops& drops() const noexcept { return writer_.export_report().drops; }  // what the file left out or rewrote (the name cut), once the save has ended
  std::uint32_t clipped() const noexcept { return resampling_ ? resampler_.clipped() : 0u; }  // outputs the resampler saturated, once the save has ended
  void begin(core::ByteSink& sink) noexcept override;
  formats::Progress step(std::size_t byte_budget) noexcept override;
  const char* failure() const noexcept override { return failure_; }
  std::uint32_t size_hint() const noexcept override { return formats::og::kInstrumentBytes + 2u * (resampling_ ? plan_.frames_out : frames_); }

 private:
  class Samples final : public core::ByteSource {
   public:
    void set(const std::int16_t* pcm, std::uint32_t frames) noexcept {
      bytes_ = reinterpret_cast<const std::uint8_t*>(pcm);
      size_ = 2u * static_cast<std::size_t>(frames);
      pos_ = 0;
    }
    core::IoResult read(void* dst, std::size_t max) noexcept override;

   private:
    const std::uint8_t* bytes_ = nullptr;
    std::size_t size_ = 0;
    std::size_t pos_ = 0;
  };
  const model::Project* project_ = nullptr;
  unsigned slot_ = 0;
  const std::int16_t* pcm_ = nullptr;
  std::uint32_t frames_ = 0;
  std::int16_t level_ = -1;
  std::int16_t fine_ = formats::WriteParams::kNoFineOverride;
  Check check_ = nullptr;
  const void* ctx_ = nullptr;
  const char* failure_ = "";
  Samples samples_;
  bool resampling_ = false;
  OgPcmPlan plan_;
  AmigaKernel* kernel_ = nullptr;
  OgResampleSource resampler_;  // (the loop it follows is bound at begin(): the model's)
  formats::og::InstrumentWriter writer_;
};

// The source of a small file already in memory (the device settings).
class BufferSaveSource final : public SaveSource {
 public:
  void set(const std::uint8_t* data, std::size_t size) noexcept {
    data_ = data;
    size_ = size;
  }
  void begin(core::ByteSink& sink) noexcept override {
    sink_ = &sink;
    pos_ = 0;
  }
  formats::Progress step(std::size_t byte_budget) noexcept override;
  std::uint32_t size_hint() const noexcept override { return static_cast<std::uint32_t>(size_); }

 private:
  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
  std::size_t pos_ = 0;
  core::ByteSink* sink_ = nullptr;
};

// What an export is asked to render: the song, or a pattern. `loaded_folder` is the project's folder as the loader knows it ("Projects/My Song", "Workspace", ""): it names the export's.
// OS-WAVEXP slice B: `form` Stems is a set of files, one per part of plan_stems(*project, what); Selection is one file of a block (`what` has its tracks and rows).
struct ExportRequest {
  enum class Form : std::uint8_t { Mix, Stems, Selection };
  audio::OfflineRequest what{};
  const char* loaded_folder = "";
  Form form = Form::Mix;
  const model::Project* project = nullptr;  // Stems: the project the set is planned from
};

// What a WAV save is asked to write.
struct WavRequest {
  const std::int16_t* pcm = nullptr;
  std::uint32_t frames = 0;
  std::uint8_t channels = 1;
  std::uint32_t rate = 44100;
  const char* prefix = "REC";            // "REC" or "SMP": the name is <prefix>_NNN
  const SampleBank* bank = nullptr;      // when the audio is a bank slot's: its version must not move while it is written ...
  unsigned slot = 0;
  const Recorder* take = nullptr;        // ... or when it is a take under review: it must stay the take (the recorder in Review with the same data)
};

// OS-STEREO step 2, slice (b), decision S2 of docs/specs/OS-STEREO.md: where the side signal S of instrument `slot` lives; the one naming rule of the loader (og_samples.cpp), the save and the load screen (it lives here
// because the name guard is the save layer's: tools/check_layers.py rule 8). `sample_path` is the path the instrument has or will have ("samples/instr01.wav"); `out` (`cap` bytes) gets "samples/instr01_s.wav"
// (the stem and "_s.wav"; the extension is matched without case). False when the path is not a "samples/<name>.wav" the save guard takes, when "<name>_s" is longer than the guard's kNameMax, when the result does
// not fit `cap`, or when another instrument's sample_path is that name (in any case): the side file would be that instrument's audio, so this one is mono. An older build never reads a file of that name (it is no
// instrument's sample_path).
bool stereo_side_path(const model::Project& project, unsigned slot, const char* sample_path, char* out, std::size_t cap) noexcept;

class SaveService {
 public:
  static constexpr std::uint32_t kSettingsDebounceMs = 1500;
  static constexpr unsigned kMaxTrackNumber = 99;
  static constexpr unsigned kMaxWavNumber = 999;
  static constexpr std::size_t kFolderChars = 40;  // "Projects/" + a name

  // OS-FILES-C C2a: how the OG export in hand is going.
  enum class OgState : std::uint8_t { Idle, Running, Done, Failed, Cancelled, Modified };

  enum class Start : std::uint8_t {
    Started,
    NoWriter,     // no card write layer attached (the simulator)
    NotWritable,  // the gate said no: gate() has the reason
    Busy,         // a save is already running
    NoJobSlot,
    NoName,       // every Track NN / REC_NNN is taken
    Empty,        // nothing to write
    TooBig,       // a WAV over 4 GiB
    CardInUse,    // something else holds the card (a firmware upload or update in progress: attach_claim)
    NameTaken,    // Save As with a typed name: a folder of that name is already in Projects/ (nothing is replaced)
    BadName,      // Save As with a typed name the save layer's guard refuses (hal/save_guard.h valid_name)
    NotSample,    // save_instrument: the slot holds no sample instrument (SAMPLE INSTRUMENTS ONLY)
    NoExport,     // no offline renderer is attached (the simulator without one, a build with no audio)
    PtiAmiga,     // save_instrument: the instrument's engine is AMIGA in a mode Paula does not play (one shot and forward loop are written, resampled: app/pti_load.h pti_check)
    PtiTooLong,   // save_instrument (OS-FILES-C C1b): the resampled audio is longer than a .pti holds (never cut)
    PtiBits,      // save_instrument: the engine is 12 BIT or 8 BIT, which a .pti cannot say
    PtiLevel,     // save_instrument: the project plays through the 1.1.14 level law and the level has no 1.2+ equivalent (under -24 dB)
  };
  // Whether something other than the app is using the card right now (the firmware update path of the console). Asked before a save starts and before the device settings are
  // written: FatFs on this volume is not re-entrant, and an update must not find a save half done. Main loop only; nullptr: nothing else uses the card.
  using Claim = bool (*)() noexcept;

  void attach(hal::storage::Storage& card, hal::storage::StorageWriter& writer) noexcept {
    card_ = &card;
    writer_ = &writer;
  }
  void attach_claim(Claim claim) noexcept { claim_ = claim; }
  // OS-FILES-C T16: a cycle counter (ticks per microsecond) for the per-stage timing of the OG export (og_timing.h). Not attached: no timing, no cost.
  void attach_cycles(OgTiming::Counter counter, std::uint32_t per_us) noexcept { og_timing_.attach(counter, per_us); }
  const OgTiming& og_timing() const noexcept { return og_timing_; }  // `diag ogexp status` prints it; reset when an export starts
  // ---- A file an app writes (OS-H4) ----
  static constexpr std::size_t kAppNameMax = hal::storage::save::kNameMax;  // the longest stem of an app's file and the longest folder name (the app's own stem)
  // An extension an app may write is guard::app_ext_ok (hal/save_guard.h). A name for a file or an app's folder is project_name_ok().
  AppExport& app_export() noexcept { return app_export_; }
  // The platform gives the staging ring of the apps' export calls once, with the card's other memory: `bytes` of SDRAM (AppExport::kRingBytes). Without it the calls answer NO_CARD.
  void attach_app_export(std::uint8_t* ring, std::size_t bytes) noexcept { app_export_.attach(*this, ring, bytes); }
  // Where the last file ended: None (not started), Running, Done, Failed. app_file() takes in a job that ended.
  enum class AppFileState : std::uint8_t { None, Running, Done, Failed };
  // Starts AppExport/<app>/<name>, <name> the first free of "<stem>.<ext>", "<stem>_001.<ext>" .. "<stem>_999.<ext>" (the stem cut so that the numbered form fits the guard's 31 bytes), as a NewOnly
  // save of `source` (a job in the runner the service last saw). `name_out` (cap bytes) gets the file name chosen. Started, or why not: Busy (a save, an export or an earlier app file runs), CardInUse,
  // NotWritable (the gate: gate() says why), NoWriter (no write layer, or no job runner seen yet), NoJobSlot, NoName (`no_suffix` true: every form is taken; false: a name or app the guard refuses).
  Start start_app_file(const char* app, const char* stem, const char* ext, SaveSource& source, char* name_out, std::size_t cap, bool& no_suffix) noexcept;
  AppFileState app_file() noexcept;
  // The running app file is given up (the app left, or went over a cap): the job is cancelled, its Temp removed, the slot let go, and app_file() says Failed. No-op when none runs.
  void drop_app_file() noexcept;
  bool attached() const noexcept { return writer_ != nullptr; }
  // Whether the card may be written now: Ok, or why not (NoCard when no write layer is attached). The gate itself: hal/storage_write.h.
  hal::storage::WriteStatus gate() noexcept { return writer_ != nullptr ? writer_->gate() : hal::storage::WriteStatus::NoCard; }
  // The gate's answer as the notification strip says it (upper case, at most 48 characters).
  static const char* gate_text(hal::storage::WriteStatus status) noexcept;

  // Saves `project`. `loaded_folder` is where the project came from ("Projects/Song", "Workspace", "": the ProjectSink's); `as_new` is Save As. Starts the job in `jobs`.
  // `as_name` (OS-FU2, with `as_new`): the folder name the user typed instead of the first free "Track NN"; NameTaken when Projects/<name> exists (never replaced), BadName when the guard
  // refuses it. nullptr: the automatic name.
  // `bank` (OS-FU2): where the audio of the project's sample instruments is; the samples the folder does not hold yet are written first (SAMPLES above). nullptr: the project file only.
  Start save_project(core::JobRunner& jobs, const model::Project& project, const char* loaded_folder, bool as_new, const char* as_name = nullptr, const SampleBank* bank = nullptr) noexcept;
  // The name the next Save As would take on its own (the first free "Track NN" in Projects/): what the on-screen keyboard starts with. False: every name is taken, or no card.
  bool suggest_name(char* out, std::size_t cap) noexcept;
  // A name the guard takes for a project folder (1..31 characters of letters, digits, space, '_', '-' and '.', no reserved device name: hal/save_guard.h).
  static bool project_name_ok(const char* name) noexcept { return hal::storage::save::valid_name(name); }
  Start save_wav(core::JobRunner& jobs, const WavRequest& request) noexcept;
  // E-SREC-09: the WAV of the take `take` (WavRequest::take) is on the card: its job ended Ok and the take is still in Review with the window that was written. True once per file; the caller then calls
  // take.note_saved() (a started job is not a saved take: a failed or cancelled one leaves the take unsaved). Main loop, as the rest of the service.
  bool take_written(const Recorder& take) noexcept;
  // OS-INSTUI S7: the sample instrument in `slot` of `project`, with the audio `bank` holds for it, as instruments/<name>.pti (PTI above). NotSample: the slot is empty, MIDI or a module;
  // Empty: it has no audio; TooBig: more audio than a .pti holds; NoName: the name and its numbered forms are all taken.
  // OS-FILES-C C1b: audio that plays at another rate (or on the Amiga engine) is written resampled to the 44,100 Hz a .pti plays at; pti_resampled_hz() is the rate it came from after a save that
  // started (0: written as it is), for the notice.
  Start save_instrument(core::JobRunner& jobs, const model::Project& project, const SampleBank& bank, unsigned slot) noexcept;
  std::uint32_t pti_resampled_hz() const noexcept { return pti_hz_; }
  // True once, for the runner's `slot` when it is the .pti save's job and it saturated samples while resampling: the app adds " (CLIPPED)" to the strip's DONE.
  bool take_pti_clipped(unsigned slot) noexcept {
    if (pti_job_ != static_cast<int>(slot)) {
      return false;
    }
    pti_job_ = -1;
    return pti_.clipped() != 0u;
  }
  // OS-STEREO (b), rule 4: true once, for the runner's `slot` when it is the project file's job of a save in which stereo instruments went down mono (a side name that collides with another instrument's sample or
  // does not fit the guard, or a side file that could not be written); `count` is how many, `first` the first one's name (upper case, valid until the next save). The app says it on the strip.
  bool take_stereo_mono(unsigned slot, unsigned& count, const char*& first) noexcept {
    if (proj_job_ != static_cast<int>(slot)) {
      return false;
    }
    proj_job_ = -1;
    count = mono_count_;
    first = mono_name_;
    return mono_count_ != 0u;
  }
  // The file name (without ".pti") an instrument called `name` is saved under: the name sanitised to what the guard takes (anything else becomes '_', no ".." and no dot or space at either end, cut to
  // `cap` - 1 bytes, at most guard::kNameMax); "Instrument NN" when nothing is left (`slot` + 1). Pure, for the tests.
  static void instrument_file_stem(const char* name, unsigned slot, char* out, std::size_t cap) noexcept;
  // OS-WAVEXP. The offline renderer and its staging ring (platform, once); then an export is a save like the others, started here. Busy while any save runs.
  void attach_export(const ExportSource::Wiring& wiring) noexcept { export_.attach(wiring); }
  bool export_attached() const noexcept { return export_.attached(); }
  Start save_export(core::JobRunner& jobs, const ExportRequest& request) noexcept;
  // Cancels the export that runs (no-op otherwise); the job's slot is Cancelled and the file is not made.
  void cancel_export(core::JobRunner& jobs) noexcept;

  // OS-FILES-C C2a, C2b: a whole project in the OG format, into Export/<project> OG[ n] (the project part is the stems' rule). One job per file, started by poll(); busy() is true until the last. Refusals as
  // save_export's (Busy, CardInUse, NotWritable), NoName when OG .. OG 99 are all taken, Empty for a project without patterns. `loaded_folder` names the project as ExportRequest::loaded_folder does.
  // `bank`: the audio of the sample instruments (null: none, and no instrument file); it must stay valid, and its audio and the document unchanged, until the export has ended.
  Start start_og_export(core::JobRunner& jobs, const model::Project& project, const char* loaded_folder, const SampleBank* bank) noexcept;
  void cancel_og_export(core::JobRunner& jobs) noexcept;
  // OS-FILES-C C2c: what an export would do now, with nothing started (the EXPORT page's OG PROJECT row and its question): the folder it would make, the files, about how many bytes and the counters of the
  // plan and the writers. The same decisions start_og_export makes, so the second F1 of the question sees what the export will count. SLICED (the C2c review's minors): og_survey_begin() takes the document
  // and touches nothing else; each og_survey_step() then does ONE small piece of work on the main loop (a few folder-name lookups on the card, the plan, then one writer's dry run: project.mt, the names
  // file, one pattern or one instrument head) and says More, Done (og_survey_result() is filled) or Failed (og_survey_why(): NoName, BadName or Empty). A document or an audio slot that changes under the run
  // restarts it. og_survey_current() is true while a finished result still describes the document. og_survey() is the whole run at once, for the tests.
  enum class SurveyStep : std::uint8_t { More, Done, Failed };
  Start og_survey_begin(const model::Project& project, const SampleBank* bank, const char* loaded_folder) noexcept;
  SurveyStep og_survey_step() noexcept;
  void og_survey_cancel() noexcept { ogs_.stage = SurveyRun::Stage::Idle; }
  bool og_survey_running() const noexcept { return ogs_.stage != SurveyRun::Stage::Idle && ogs_.stage != SurveyRun::Stage::Done && ogs_.stage != SurveyRun::Stage::Failed; }
  bool og_survey_current() const noexcept { return ogs_.stage == SurveyRun::Stage::Done && !ogs_.plan.changed(); }
  const OgSurvey& og_survey_result() const noexcept { return ogs_.out; }
  Start og_survey_why() const noexcept { return ogs_.why; }
  Start og_survey(const model::Project& project, const SampleBank* bank, const char* loaded_folder, OgSurvey& out) noexcept;
  // The words for a refusal of start_og_export / og_survey (upper case, for the strip and the console); "COULD NOT START" for anything else.
  static const char* og_start_text(Start s, hal::storage::WriteStatus gate) noexcept;
  // The runner's slot of the OG file that runs (-1: none): App::report_jobs keeps the files' jobs quiet, the App's og pass words the end.
  int og_job_slot() const noexcept { return kind_ == Kind::OgExport ? slot_ : -1; }
  OgState og_state() noexcept {
    reconcile();
    return og_.state;
  }
  unsigned og_files_done() const noexcept { return og_.done; }
  unsigned og_files_total() const noexcept { return og_.plan.total(); }
  const char* og_folder() const noexcept { return og_.folder; }  // "Export/Song OG", "" before the first export
  const char* og_why() const noexcept { return og_.why; }        // after Failed: the job's reason; after Modified: what changed
  const formats::og::ExportDrops& og_drops() const noexcept { return og_.drops; }
  // C2b: what the levels in the files are, in the strip's words ("" before the first export): LEVELS WRITTEN AS PLAYED (1.1.14 -> 1.2+) when any level was converted or clamped, else LEVELS WRITTEN AS STORED.
  const char* og_levels() const noexcept;
  std::uint32_t og_clipped() const noexcept { return og_.clipped; }  // outputs the resampler saturated to 16 bits, over the files finished
  // OS-FILES-C C2c, T16 (the cost of the export on the main loop): the longest single step of any file's job and the steps that overran their slice, over the files finished (core::JobStats of each, taken by
  // App::report_jobs when the job ends; `diag ogexp status` prints them). 0 before the first.
  void og_note_job(const core::JobStats& stats) noexcept {
    og_.max_step_us = stats.max_step_us > og_.max_step_us ? stats.max_step_us : og_.max_step_us;
    og_.overruns += stats.overruns;
  }
  std::uint32_t og_max_step_us() const noexcept { return og_.max_step_us; }
  std::uint32_t og_overruns() const noexcept { return og_.overruns; }
  // The export in progress: its source (the pass and the progress for the overlay) and the runner's slot of its job (-1 when none runs; still valid in the pass in which the job ended, until poll()
  // reconciles it: App::report_jobs reads it to word the end).
  const ExportSource& export_source() const noexcept { return export_; }
  int export_slot() const noexcept { return kind_ == Kind::Export ? slot_ : -1; }
  // The name an export would be made under now, "Export/<project>/<stem>.wav" (for the dialog before it starts; a set of stems: "Export/<project>/<set>_*.wav"); false when the card or the names do not allow.
  bool export_name(const ExportRequest& request, char* out, std::size_t cap) noexcept;
  // A set of stems (slice B). The job that ended is taken in (the stem is counted, the plan goes on or ends); export_more says another file is still to start (not after a cancel), export_next starts
  // it (Started; NoJobSlot: try again next pass, poll does; anything else: the set has ended). stem_index() is the file in progress, 1-based (0 when there is no set), stems_total() the files of the
  // set, stems_saved() the files finished. The three stay valid after the set has ended, until the next export starts.
  void export_settle() noexcept { reconcile(); }
  bool export_more() const noexcept { return stems_.active && stems_.next < stems_.plan.count && !stems_.stop; }
  Start export_next(core::JobRunner& jobs) noexcept;
  unsigned stem_index() const noexcept { return stems_.next; }
  unsigned stems_total() const noexcept { return stems_.plan.count; }
  unsigned stems_saved() const noexcept { return stems_.done; }
  bool stems_active() const noexcept { return stems_.active; }
  // The label of the file in progress ("T03", "MIX"), "" when there is none.
  void stem_now(char* out, std::size_t cap) const noexcept;

  // ---- DELETE (OS-FILES part B; DELETE above) ----
  // One call to the writer (a few milliseconds; not a job), refused while a save runs. `loaded_folder` is where the open project came from (ProjectSink::folder() of a load that worked, "" otherwise):
  // the project the user is in is never marked.
  enum class Move : std::uint8_t {
    Done,
    NoWriter,     // no card write layer attached
    NotWritable,  // the gate said no (gate_text)
    Busy,         // a save is running
    CardInUse,    // the firmware update path has the card
    OpenProject,  // the folder is the open project's
    BadName,      // not a name the guard takes (a name of an OG project the layer cannot write, too)
    NotFound,     // the folder is not there
    Failed,       // anything else: move_status() says what the card said
  };
  // DELETE: marks Projects/<name> with the empty file .deleted (Done also when it was marked already). The project's entry leaves the RECENT list.
  Move delete_project(const char* loaded_folder, const char* name) noexcept;
  // Does "Projects/<name>" (a load's folder) hold the DELETE marker? The File screen's open asks it before every load (folder_marked_deleted, app/dir_scan.h).
  bool project_deleted(const char* folder) noexcept { return card_ != nullptr && folder_marked_deleted(*card_, folder); }
  // What the card said when the last move was Failed.
  hal::storage::WriteStatus move_status() const noexcept { return move_status_; }
  // The folder name (under Projects/) of the project the user is in: the one saved last while it is the open document, else the folder it was loaded from. False when it has none.
  bool open_project_name(const char* loaded_folder, char* out, std::size_t cap) const noexcept;
  // ---- RECENT (OS-FILES part B) ----
  const RecentList& recent() const noexcept { return recent_; }
  // Reads Settings/recent.o3rc (or its backup) when this card's has not been read, unless changes in memory are waiting to be written. A missing or damaged file is an empty list.
  void recent_refresh() noexcept;
  // The project at "Projects/<name>" was opened (a load that worked): it is the newest. A saved project is added by the service itself.
  void note_opened(const char* folder) noexcept;
  // A name the File screen did not find on the card: it leaves the list, and the file is rewritten by the next change.
  void recent_forget(const char* name) noexcept { (void)recent_.drop(name); }

  // Once a pass: the bookkeeping of a save that ended, and the device settings (saved by themselves when they have changed and stayed still). `now_ms` is uptime.
  void poll(core::JobRunner& jobs, const model::Settings& device, std::uint64_t now_ms) noexcept;
  // The slot of a job the notification strip stays quiet about while it runs and when it succeeds.
  bool quiet_slot(unsigned slot) const noexcept { return quiet_slot_ == static_cast<int>(slot); }
  // The slot of a job that says nothing at all, not even that it failed (the RECENT file: "a write failure is silent: it costs only the list").
  bool silent_slot(unsigned slot) const noexcept { return silent_slot_ == static_cast<int>(slot); }

  // ---- The document the user is in (OS-007o) ----
  // Whether the project holds changes no save has taken (F3 NEW asks before it throws them away): its generation (any accepted mutation) is not the one of the last time the
  // document was known to be on the card or fresh. A save that ended well makes it the one the save was started from (a project that changes while it is written does not
  // save: the writer checks); a load or a new project makes it the generation after the load. Undoing back to the saved state still counts as changed, which errs on the safe side.
  bool dirty(const model::Project& project) noexcept;
  // The App has a document: the first one seen is the baseline (the platform's own at boot, which no one has edited; a rebuilt App does not forget what was edited).
  void watch(const model::Project& project) noexcept;
  // The document was replaced (a project loaded, or a new one made): its generation is the baseline, and the project the user is in is no project of Projects/ yet, so SAVE will not
  // replace the folder of the one before it. `unsaved`: the document is new work that is on no card (an imported module): dirty() stays true until a project save ends well, whatever
  // the generation does.
  void document_replaced(const model::Project& project, bool unsaved = false) noexcept;

  // The last save that started: the folder or file ("Projects/Track 01", "Samples/REC_001.wav") for a message.
  const char* last_name() const noexcept { return last_[0] != '\0' ? last_ : ""; }
  // A save, a stem export or the like is running (one state for every job kind). The RECENT file's write is not counted: it is quiet and small, nothing waits for it (a save or a move that comes while it
  // runs stops it, and it is written again when the card is idle: yield_recent).
  bool busy() const noexcept { return occupied(); }
  // OS-USBDISK U2a (4.2, Q9): the RECENT list or the device settings differ from the card's (a write is due, in its debounce or running). The hand-over waits for it (up to 2 s), then refuses.
  bool write_due() const noexcept { return recent_.generation() != recent_saved_ || settings_sig_ != settings_saved_ || kind_ != Kind::None || job_.running(); }
  // The card is being handed to a computer: no RECENT or settings write starts (a pass of poll() leaves them dirty). Main loop.
  void hold_background(bool on) noexcept { hold_ = on; }
  // ---- OS-POWER P1: the shutdown (docs/specs/OS-POWER.md 4.1 B) ----
  // The shutdown has begun: no job starts from here but the device settings' own flush, which is written NOW instead of 1.5 s after the last change (the same writer, the same file, the same quiet job);
  // the RECENT file waits; a set of stems or an OG export ends with the file in flight (nothing starts after it); a project plan in progress goes on to its end (the save in flight is finished).
  void power_begin() noexcept;
  // A save job runs, a project plan is between two of its jobs, or the settings flush is still to be written (and has not been given up): what the shutdown waits for in stage B.
  bool power_busy() noexcept;
  // The job in flight has begun to read back or rename: a cancel is the rollback of a nearly finished save, so the shutdown waits for it (up to its hard cap) instead.
  bool power_committing() const noexcept { return job_.running() && job_.past_write(); }
  // Stage B at its cap: the job in flight is cancelled (its Temp removed, the old file whole), nothing follows it and the settings flush is given up.
  void power_cancel() noexcept;
  // The runner's slot of the job in flight (-1: none), for the app to leave it alone when it cancels the other jobs.
  int job_slot() const noexcept { return job_.running() ? slot_ : -1; }
  // OS-BUG-REMOUNT: the card was taken out, put back or handed to a computer (App::card_changed). The volume serial does not say the files are as they were (a computer may have replaced or erased
  // Projects/X/samples/kick.wav), so what the saves and the bank believe the folder holds is checked against the files (size and modification stamp: one directory lookup a sample, not a
  // CRC) at the next save before a sample is skipped; the RECENT list is read again (unless changes in memory wait for their write). A sample job that has finished is taken in first, so that
  // its belief is recorded before the check starts (it cannot be re-recorded under the old card afterwards).
  // RESIDUAL: there is no RTC (FF_FS_NORTC=1), so every file this OS writes carries one fixed stamp; a computer's edit is told by its new modification time or its other size. A same-size copy whose
  // modification time was preserved (or equals the fixed stamp) goes undetected: the sample is skipped and the card keeps the computer's file. Hardware check: QA-FIL-091.
  void card_remounted() noexcept;
  const AtomicSave& job() const noexcept { return job_; }

 private:
  enum class Kind : std::uint8_t { None, Project, Wav, Settings, Sample, Instrument, Export, Recent, AppFile, OgExport };
  struct Watch {
    const SampleBank* bank = nullptr;
    unsigned slot = 0;
    std::uint32_t version = 0;
    const Recorder* take = nullptr;
    const std::int16_t* pcm = nullptr;
    std::uint32_t frames = 0;
  };
  static bool watch_ok(const void* ctx) noexcept;
  // OS-FU2, a project's samples: what a save in progress still has to do. The project file is the last job of the plan; `advance_plan` (poll) starts the next one.
  struct Plan {
    bool active = false;
    bool pinned = false;             // the writer has been told the save began (begin_save): once, at its first job
    const SampleBank* bank = nullptr;
    const model::Project* project = nullptr;
    std::uint64_t todo = 0;          // bit n: slot n is still to be written
    bool replace = false;            // the project file replaces one (a Save), else NewOnly (a Save As)
    bool from_folder = false;        // the project was loaded from this very folder (OS-BUG-OGSAVE: its files may already hold the audio)
    char name[hal::storage::save::kNameMax + 1] = {};
    char anchor[64] = {};
  };
  // OS-STEREO (b), rules 4 and 8, what a project save owes the side files: `todo` the slots whose side file is to be written, `ok` the slots whose stereo flag may go into the project file (bit n set: its side file is
  // held by the folder or written by this save; every bit but those of the flagged slots without one), `refused` the flagged slots with a side signal whose side name was refused (they are saved mono: the strip says so).
  struct SidePlan {
    std::uint64_t todo = 0;
    std::uint64_t ok = ~std::uint64_t{0};
    std::uint64_t refused = 0;
  };
  SidePlan side_;  // the running plan's (side_.todo: the side files still to be written, after every mid), or the last project save's
  std::uint64_t samples_to_write(const model::Project& project, const SampleBank& bank, const char* name, bool read_from_folder, SidePlan& side) const noexcept;
  // A flagged slot saved without its side file: counted for the strip (take_stereo_mono), the first one named (the instrument's name, upper case).
  void note_mono(const model::Project& project, unsigned slot) noexcept;
  Start advance_plan(core::JobRunner& jobs) noexcept;
  // One sample of the plan as a job: its mid, or (`side`) its side file.
  Start start_sample(core::JobRunner& jobs, unsigned slot, bool side) noexcept;
  // OS-FILES part B
  // OS-FILES part B
  Move move_ready() noexcept;
  void forget_dir(const char* name) noexcept;                   // a project folder of this name was moved: what is remembered of it goes
  bool occupied() const noexcept { return (job_.running() && kind_ != Kind::Recent) || plan_.active || stems_.active || og_.active; }
  // A job that steps aside for the user: the RECENT file, and an app's file that is still being written. launch() cancels it once the request really starts (yield_recent). An app's file that has begun
  // to commit (read-back, rename) is not cancelled: the request answers Busy until it has ended, which takes a moment.
  bool yieldable() const noexcept { return job_.running() && (kind_ == Kind::Recent || (kind_ == Kind::AppFile && !job_.past_write())); }
  // What can stop a request of the user: occupied() without the jobs that step aside for it.
  bool user_blocked() const noexcept { return (job_.running() && !yieldable()) || plan_.active || stems_.active || og_.active; }
  void yield_recent() noexcept;  // the RECENT file's write, or an app's file still being written, is cancelled (the RECENT file is asked for again later): the user's request is starting its job
  void recent_touch(const char* name) noexcept;

  Start launch(core::JobRunner& jobs, const char* dir, const char* name, AtomicSave::Mode mode, SaveSource& source, const char* label, Kind kind, bool quiet,
               const char* verb = "SAVE ", HeldCheck* held = nullptr) noexcept;
  bool export_names(const ExportRequest& request, char* dir, std::size_t dir_cap, char* file, std::size_t file_cap, char* label, std::size_t label_cap) noexcept;
  bool export_project(const ExportRequest& request, char* project) noexcept;
  bool stem_names(const ExportRequest& request, char* dir, std::size_t dir_cap, char* prefix, std::size_t prefix_cap) noexcept;
  Start start_stems(core::JobRunner& jobs, const ExportRequest& request) noexcept;
  Start start_stem(core::JobRunner& jobs) noexcept;
  Start advance_stems(core::JobRunner& jobs) noexcept;
  Start start_og_file(core::JobRunner& jobs) noexcept;
  Start og_pick_folder(const char* loaded_folder, char* path, std::size_t path_cap, char* label, std::size_t label_cap) noexcept;
  Start og_folder_try(const char* name, unsigned n, char* path, std::size_t path_cap, char* label, std::size_t label_cap) noexcept;
  Start advance_og(core::JobRunner& jobs) noexcept;
  void end_og(OgState state, const char* why) noexcept;
  bool free_name(char* out, std::size_t cap, const char* dir, const char* prefix, const char* suffix, unsigned digits, unsigned first, unsigned last, unsigned& cursor) noexcept;
  void reconcile() noexcept;
  void forget_other_card() noexcept;
  void end_save() noexcept;  // the save (one job, or a plan's all) is over: the writer lets go of the card it pinned
  static std::uint32_t signature(const model::Settings& device) noexcept;

  hal::storage::Storage* card_ = nullptr;
  hal::storage::StorageWriter* writer_ = nullptr;
  Claim claim_ = nullptr;
  AtomicSave job_;
  Kind kind_ = Kind::None;
  int quiet_slot_ = -1;
  int silent_slot_ = -1;
  AppFileState app_result_ = AppFileState::None;  // how the last app file ended (reconcile)
  int slot_ = -1;  // the runner's slot of the job in progress
  core::JobRunner* jobs_ = nullptr;  // the runner the service last started a job in (poll, launch): yield_recent cancels the RECENT job in it
  NativeSaveSource native_;
  WavSaveSource wav_;
  PtiSaveSource pti_;
  int pti_job_ = -1;  // the runner's slot of the .pti save in progress (take_pti_clipped)
  int proj_job_ = -1;                              // the runner's slot of the project file's job (take_stereo_mono)
  unsigned mono_count_ = 0;                        // OS-STEREO (b): instruments of this project save written mono although they were stereo (their side name was refused or their side file could not be written)
  char mono_name_[model::kInstrumentNameMax + 1] = {};  // the first of them
  std::uint32_t pti_hz_ = 0;  // save_instrument: the rate the .pti in progress was resampled from, 0 when it is written as it is
  BufferSaveSource buffer_;
  BufferSaveSource recent_source_;
  ExportSource export_;
  AppExport app_export_;
  std::uint8_t settings_[formats::settings::kMaxFileBytes];
  std::uint8_t recent_buf_[RecentList::kMaxFileBytes];
  // The project the user is in: the name of its folder under Projects/, and the loaded folder it was set for (it stops applying when another project is loaded).
  char current_[hal::storage::save::kNameMax + 1] = {};
  char anchor_[64] = {};
  char pending_name_[hal::storage::save::kNameMax + 1] = {};
  char pending_anchor_[64] = {};
  char last_[hal::storage::save::kPathCap] = {};
  bool power_ = false;            // OS-POWER P1: power_begin() was called (never cleared)
  bool power_gave_up_ = false;    // ... and the settings flush was given up (power_cancel)
  bool hold_ = false;             // hold_background()
  bool settings_clean_ = true;    // the device settings on the card are the ones in memory, as of the last poll()
  bool clean_valid_ = false;
  bool unsaved_ = false;                  // document_replaced(…, true): dirty until the project is saved
  std::uint32_t clean_generation_ = 0;    // the project's generation when it was last known to be on the card or fresh
  std::uint32_t pending_generation_ = 0;  // the generation the project save in progress was started from
  unsigned next_track_ = 1;
  unsigned next_rec_ = 1;
  unsigned next_smp_ = 1;
  // The device settings: a signature of their generations at the last pass, when it last moved, and the one that is saved (or being saved).
  bool settings_seen_ = false;
  std::uint32_t settings_sig_ = 0;
  std::uint32_t settings_saved_ = 0;
  std::uint32_t settings_inflight_ = 0;
  std::uint64_t settings_moved_ms_ = 0;
  // OS-FILES part B. The recent projects and the file's last state: the generation written (or read), the card it was read from.
  RecentList recent_;
  std::uint32_t recent_saved_ = 0;
  std::uint32_t recent_card_ = 0;
  bool recent_loaded_ = false;
  hal::storage::WriteStatus move_status_ = hal::storage::WriteStatus::Ok;
  Watch watch_;  // what the WAV in progress must still be the audio of
  bool wav_take_done_ = false;  // the last WAV job, a take's, ended Ok (reconcile); take_written() reads and clears it
  Plan plan_;
  // OS-WAVEXP slice B: a set of stems in progress. Like the project's plan the set keeps the card it began on (launch pins it); `plan` and the counts stay readable after it ends.
  struct StemRun {
    bool active = false;
    bool stop = false;               // F8: no file after the one in progress
    unsigned next = 0;               // the file to start next (= the number started)
    unsigned done = 0;               // files finished
    std::uint32_t length = 0;        // the set's length in frames, known once the first file's measuring pass has ended (0: not yet)
    audio::OfflineRequest mix{};     // the plain render the length is measured with
    char dir[hal::storage::save::kPathCap] = {};
    char prefix[hal::storage::save::kNameMax + 1] = {};  // "<project>_Song_S0001_": what each file's label and ".wav" follow
    StemPlan plan;
  };
  StemRun stems_;
  // OS-FILES-C C2a: an OG export in progress (like the stems' set it keeps the card it began on; `done`, `state` and the rest stay readable after it ends).
  struct OgRun {
    bool active = false;
    bool stop = false;        // cancel: no file after the one in progress
    OgState state = OgState::Idle;
    unsigned done = 0;        // files finished
    OgExportPlan plan;
    OgFile file;              // the file of the job in progress
    OgFileSource source;
    const SampleBank* bank = nullptr;
    std::uint32_t clipped = 0;
    std::uint32_t max_step_us = 0;  // T16: the longest step of a file's job so far
    std::uint32_t overruns = 0;
    formats::og::ExportDrops drops;
    const char* why = "";
    char folder[hal::storage::save::kPathCap] = {};  // Export/<project> OG[ n]
    char label[hal::storage::save::kNameMax + 1] = {};  // the folder's name, for the strip
  };
  OgRun og_;
  OgTiming og_timing_;  // (T16) main loop only: the stages of the OG export and of its survey
  // The sliced survey (og_survey_begin / og_survey_step): its own plan, so that a survey never touches the export's; main loop only.
  struct SurveyRun {
    enum class Stage : std::uint8_t { Idle, Folder, Plan, Writers, Done, Failed };
    Stage stage = Stage::Idle;
    Start why = Start::Started;
    const model::Project* project = nullptr;
    const SampleBank* bank = nullptr;
    unsigned n = 1;                                      // the next folder name to look up (OG, OG 2 ..)
    char name[hal::storage::save::kNameMax + 1] = {};    // the project part of the folder name
    char path[hal::storage::save::kPathCap] = {};
    OgExportPlan plan;
    OgSurvey out;
  };
  SurveyRun ogs_;
  // The audio each slot's file holds in Projects/<known_dir_>/samples, as the bank's version of it (0: unknown): what earlier saves wrote, so that a later one skips it.
  char known_dir_[hal::storage::save::kNameMax + 1] = {};
  std::uint32_t known_[64] = {};
  std::uint32_t known_side_[64] = {};  // OS-STEREO (b): the same for each slot's side file (<stem>_s.wav)
  hal::storage::FileStamp known_file_[64] = {};  // OS-BUG-REMOUNT: what Projects/<known_dir_>/samples/<file> looked like when that save finished (size 0 and stamp 0: unknown)
  std::uint32_t remount_epoch_ = 0;  // card_remounted() calls so far; save_epoch_: the value when the running save checked its samples (an event in between leaves the beliefs to be checked again)
  std::uint32_t save_epoch_ = 0;
  bool revalidate_ = false;                       // the card was out and back: a belief about a file stands only while the file still looks as it did (cleared by the next project save that completes)
  char smp_path_[hal::storage::save::kPathCap] = {};            // the file of the sample job in progress
  std::uint32_t known_card_ = 0;  // the writer's card_id() that known_ was written on: another card forgets it (OS-FU5)
  std::uint32_t smp_card_ = 0;    // the card of the sample job in progress
  bool smp_side_ = false;   // OS-STEREO (b): the sample job in progress writes the slot's side file
  unsigned smp_slot_ = 64;  // the slot of the sample job in progress, and the version it was started on
  std::uint32_t smp_version_ = 0;
};

// Reads Settings/device.o3st into `out` (the device settings: made the defaults first, and left so on any error, a missing file included). Read only; true when a file was
// read and taken (a file that is missing or damaged is not an error for the device: the defaults stand). The platform calls it at boot, after the card mounted.
bool load_device_settings(hal::storage::Storage& card, model::Settings& out) noexcept;

// OS-QAFIX-E-FILE-10 (maintainer, 2026-10-22): the project the unit opens at power-on. The newest RECENT project (Settings/recent.o3rc, or the .bak a cut write leaves) that is still on the card: `folder` gets
// "Projects/<name>" (at most `cap` - 1 characters), `native` says which file it holds (project.o3pj: true; project.mt: false). False when the list is empty, unreadable or names nothing that has a project file
// (a folder that is gone, or one a DELETE hid): the caller then opens Workspace as before. Read only; the card is the caller's (the save layer is not up yet at boot).
bool boot_recent_project(hal::storage::Storage& card, char* folder, std::size_t cap, bool& native) noexcept;

}  // namespace o3os::app
