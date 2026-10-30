// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The sample memory as the sample screens see it (OS-011a, new Community OS behaviour): which PCM each instrument slot has in the sample arena (SDRAM), a way to
// put a new one there, and the one safe way to tell the voices about it. The loaders bind the voices' table themselves while a project loads (og_samples.h:
// the transport is stopped and the voices are quiet); this class is for what happens afterwards, while the user works: Sample load (a WAV into a slot), Wave /
// Chop (points and slices of the instrument), Sample edit (new PCM).
//
// WHAT IT KNOWS. For every slot (the voices' table has 64; slot n is instrument n + 1): the whole sample as it sits in the arena (pointer and frames, before the
// instrument's start and end cut a window out of it: the voices' SampleDesc only knows the window), and a version that moves whenever that audio is replaced (a
// peak cache of it is stale when it differs). The loaders and the demo kit say where they put what (clear / adopt / set_used); a WAV loaded from the card goes to
// the free tail of the arena (reserve / commit_load).
//
// THE ARENA is a bump allocator: it only grows, from the start (what the loaders place) upward. A project load clears it. Replacing a slot's audio does not give
// the old region back at once (voices may be reading it): that is the price of never rewriting a sample in place, and the screens show the free space so that it is
// seen. The room of a replaced block is reused in two cases (OS-FU1, OS-ARENA-RECLAIM, RECLAIM below); the slack of a block bigger than its audio stays the slot's. Sample edit reuses a
// small pool of buffers instead (sample_edit.h), so its undo does not eat the arena.
//
// RECLAIM (OS-FU1, OS-ARENA-RECLAIM; docs/specs/OS-ARENA-RECLAIM.md). Slot::own / own_len remember the block a slot owns and its capacity: the one commit_load() gave it, or the one a
// loader placed (adopt's `block`); a take's window or an edit's buffer is not ours to move. When commit_load() replaces such audio (the slot still reads its own block, no other holder does),
// Slot::stale records the old block and the slot is marked in SampleBankMem::reclaim_marks. Then, in the SAME quiet window as the descriptors (poll(): transport stopped, no live note,
// voices silent for kSettleMs, and `jobs_idle`: no job reads a sample), after the slot has been rebound to the new audio: if the new block is still the last of the arena (nothing was
// allocated after it: no slice table, edit buffer or take), still the slot's audio, short enough to move in a pass (kReclaimMax), no undo or redo step lies in the old block and no other
// holder reads the old block or the new one, the slot is made silent and the new audio is moved (memmove) to the start of the old block, in one of two cases: (a) the old block ends where
// the new one starts: the arena shrinks to the end of the moved audio and the capacity is its length (OS-FU1); (b) the new audio fits the old block's capacity (a project's sample replaced by a
// shorter WAV, or the same slot again and again): the arena shrinks to where the new block began, so the load cost nothing, and the old block stays the slot's capacity. In both the version
// moves (a peak cache is stale) and the slot is rebound at its new place. No voice, live or sequenced, can read either block meanwhile: that is the handover's own guarantee. Any other layout
// keeps the old block until the next project load. Giving the slack to other slots needs a hole list or a compaction of the tail with reference fix-up: not done.
//
// THE HANDOVER (the protocol the review asks for). The voices read a slot's SampleDesc in the render context (a note on, the filter and the amp envelope while a
// voice plays), and set_sample() writes it in the main loop in several words: the voices' own contract is "a slot is never rewritten under a playing voice"
// (voices.h). So nothing here writes the table at once. request_rebind(slot) only marks the slot; poll() (once a pass, main loop) applies the marks when
//   (1) the transport is stopped (the caller says so: the sequencer is the only thing that starts a SEQUENCED note), and
//   (2) no LIVE note is queued or held (the caller says so: `live_idle`, PreviewSink::live_idle: the live queue is empty and the render's last look at its table of held
//       notes found none). Live notes (OS-005g: pads, MIDI keyboard, previews) are pushed by this same main loop, possibly earlier in the very pass that polls, and a
//       note still in the queue is a voice that starts at the next block: so the mark waits for it, and it plays on the descriptor the engine already has (valid memory,
//       below). The main loop is the queue's only producer and does not push while it is inside poll(), so an empty queue stays empty until poll() returns, and
//   (3) the voice pool has been silent (voices_in_use() == 0, a benign read of the render context's state, as OS-007f's Settle reads it) for kSettleMs, which is
//       longer than a block (1.45 ms) plus a margin: a block that was already running when the transport stopped has ended.
// Each pass re-checks all three and a failed check restarts the settle, so a note pushed while a mark is pending (a pad pressed, a key struck) holds the handover back
// until it has ended; (2) and (3) are read in that order (queue first: after it is empty nothing new can start, so a pool seen silent afterwards stays silent).
// Only then are the descriptors rebuilt from the project's instruments and the PCM of the slots (make_sample_desc) and written with set_sample(). Until then the
// engine keeps playing what it had: the old PCM is still valid memory (nothing is ever freed or overwritten while it may be played), the new one is not
// referenced. When the swap is made nothing is playing, so no voice, live or sequenced, reads the PCM it replaces and a buffer that only the old descriptor named
// (alive(): "bound") may be reused from then on; before it, alive() keeps every buffer the engine's descriptor names. The cost of the protocol is that an edit reaches the
// sound when playback is stopped and the pads are let go (the screens say "applies when stopped"). That holds for what the descriptor takes from the PCM: the audio, the window (start and end), the loop mode and the slices.
// The loop's START and END are the exception (OS-MOD, LIVE LOOP, voices.h): a Wave edit that moves a loop point inside the same window and mode sends the new point to the render as a ParamChange
// (og_samples.h live_loop_moves) and the render, the only writer of the descriptor while a voice plays, moves the descriptor's loop and the voices that loop over it. The handover then rebuilds the same
// loop from the project (it waits for the ring to be empty, so the render has applied every move), which is what keeps the two writers apart: the main loop writes a descriptor only when no voice
// sounds and nothing is in the ring.
// STAGED DESCRIPTOR (OS-BUG-SLICEPITCH). The wait above was too long for a note: after ADD, EVEN, AUTO, their undo, a delete, a play-mode or loop-direction change or a load, a pad or a step started while
// the transport ran or a pad was held played the OLD descriptor (about 3 octaves down for a freshly chopped slot) until silence. So poll() also STAGES the new descriptor, with none of the three
// conditions: it builds it from the project exactly as rebind_now() does, into the cell of VoicePool (voices.h, Staged descriptor), and the render takes it at the start of its next block, before it
// starts any note: every note on from then on reads the new descriptor, and the slot's sounding voices end with a declick only when their audio or kind changed. What stays as it was: the slot keeps its
// pending_ bit, and the quiet handover rewrites the same descriptor later, moves `bound` to the new audio and runs RECLAIM, so the old block stays protected for as long as a voice may read it. Staging
// needs the ParamChange ring empty (the descriptor is built from the project, and a queued change that names the slot was made against the old one), the tail free of an import or a take (a slice table is
// taken from it), and one free cell; a cell the render does not take within kStageTimeoutMs (no render is running) is given back. Known limits: a live note pushed in the same pass BEFORE the
// poll() that stages (up to one block) plays the old descriptor; a Params edit made between two polls reaches the new descriptor only through its own ParamChange (the ring is drained after the take).
// The instrument's PARAMETERS (volume, pan, tune, filter, the amp envelope and the cutoff modulator: a Params edit) have a live path as well (OS-LIVEP): the ParamChange the edit
// sends updates those fields of the engine's descriptor in the render context (VoicePool::apply_param, voices.h LIVE PARAMETERS) and the voices that play the slot take them at the next
// block; the handover is then the safety net, and the two agree because the descriptor is built from the project with the same function (audio::apply_instrument_param).
//
// THE CLIPBOARD AND SHARED AUDIO (OS-INSTUI S7). COPY puts one instrument and a REFERENCE to its slot's audio (pointer and length, no copy) on the clipboard; PASTE gives the destination slot the
// same pointer: two slots (or a slot and the clipboard) read one block of the arena, which is never rewritten in place, so a share costs no arena. There is no stored count: how many holders a
// block has is read by scanning (pcm_refs: the slots, the clipboard, the swap records below), so nothing can drift out of step with undo, a delete or a reload. What keeps a block alive is what
// already did (alive()): a slot's audio, the engine's descriptor, the sample edit's history, and now the clipboard and the swap records; a spare edit buffer is reused only when none holds it.
// What moves audio is guarded the same way: RECLAIM (above) moves and overwrites blocks, so it is refused when another holder reads the block that would move or the one that would be written over.
// Each holder has its own version (a peak cache is keyed by slot and version): a paste bumps the destination's, the source's does not change, and an edit of either gives that slot a new block
// and a new version while the other keeps the old block.
// UNDO. The instrument is the model's (one undo unit); the audio a paste or a load put in the slot is the bank's. So a paste / load that changes a slot's audio records a SWAP (slot, the audio
// before, the audio after), and undo / redo of that unit (App: undo_or_redo, which knows the unit's action) calls audio_undone / audio_redone, which put the slot's audio back or again and move
// its version. The records are two bounded stacks (kSwaps each: done, undone), newest first; a new paste or load empties the undone one (the journal drops its redo units the same way). A swap that
// has been pushed out of the stack (more than kSwaps units back) is replaced by a mark (Swap::valid false) when its unit is undone or redone, so that the pairing holds: the instrument comes back with the audio the slot has then (nothing is lost: the block is still in the arena), and no newer swap of the slot is taken for it.
//
// THE SIDE SIGNAL (OS-STEREO step 2, slice (b); docs/specs/OS-STEREO.md). A stereo sample is its mid M, the block every mono path reads, and a side S = L - M that follows it in the SAME block: M at
// pcm[0 .. frames), S at pcm[even(frames) .. 2 * even(frames)). Slot::side (and Audio::side, Step::side) points at S, nullptr for every mono sample; a block with S is 2 x even(frames) samples long (own_len
// too). commit_load() and adopt() take S with M as one unit: it is never added to a block later. What carries S: a share (paste, the clipboard), a swap record (undo of a load or paste), an edit step (undo of an
// edit); what drops it: a new mono audio in the slot (an edit's result, a take, a mono load), which the edit's undo puts back. RECLAIM never moves a block that has S (rule 5 of the risk review: the new audio, or
// the one it would replace, is then kept where it is). The voices do not read S yet (slice (c)): a stereo instrument still plays M.
//
// Contexts: main loop only. No heap.

#include <o3os/app/og_folder.h>
#include <o3os/app/preview_job.h>
#include <o3os/app/pti_load.h>
#include <o3os/app/sample_edit_job.h>
#include <o3os/app/slice_job.h>
#include <o3os/audio/smoothing.h>
#include <o3os/audio/voices.h>
#include <o3os/formats/format.h>
#include <o3os/formats/iff_sample.h>
#include <o3os/hal/storage.h>
#include <o3os/formats/wav_sample.h>
#include <o3os/formats/wt_surge.h>
#include <o3os/app/transient.h>
#include <o3os/app/wave_math.h>
#include <o3os/model/project.h>

#include <cstddef>
#include <cstdint>

namespace o3os::app {

// The bank's big parts, apart from it so that the platform keeps them in SDRAM (the main loop's SRAM is nearly all spoken for): the table of slots, the edit history's
// steps, the WAV reader (1.4 KB) and the slicer's hits (1 KB), about 5 KB in all. The bank holds a pointer to it; whoever calls init() gives it (the device places one in the
// tail of the sample arena, a test makes one on the heap). Constructed with `new (place) SampleBankMem()`.
struct SampleBankMem {
  static constexpr unsigned kSlots = audio::kSampleSlots;
  static constexpr unsigned kHistory = 3;
  struct Slot {
    std::int16_t* pcm = nullptr;
    std::uint32_t frames = 0;
    std::int16_t* side = nullptr;   // OS-STEREO (b): the side signal S at pcm + even(frames) (frames long), nullptr: a mono sample
    std::uint32_t version = 0;
    std::int16_t* bound = nullptr;  // the audio the voices' descriptor was last built over: the engine may be playing it, so nothing may overwrite it
    std::int16_t* table = nullptr;  // the slot's slice table in the arena (kTableSamples), made when a beat slice instrument first needs it
    std::int16_t* own = nullptr;    // OS-FU1: the arena block commit_load() or a loader gave the slot's current audio (nullptr: some other origin, never moved or freed)
    std::uint32_t own_len = 0;      // its capacity in samples (even; the audio may be shorter, OS-ARENA-RECLAIM)
    std::int16_t* stale = nullptr;  // the block that commit_load() replaced, below the slot's new audio: its room is reused at the handover when it can be
    std::uint32_t stale_len = 0;
    std::uint32_t card_size = 0;     // OS-BUG-REMOUNT: the size and modification stamp of that WAV when the loader read it (hal::storage::FileStamp): after the card was out and back, the belief stands only while they still match
    std::uint32_t card_stamp = 0;
    std::uint32_t card_version = 0;  // OS-FU2: the version of the audio a project loader read from the project's own samples/<file>.wav (0: none); a save skips what is still it
  };
  // One step of the edit history: the version on the other side of the edit (the old one in the undo steps, the new one in the redo steps) and the points around it.
  struct Step {
    std::int16_t* pcm = nullptr;
    std::uint32_t frames = 0;
    std::int16_t* side = nullptr;  // the version's S (OS-STEREO (b))
    PointsRec before;
    PointsRec after;
  };
  Slot slots[kSlots];
  // The recorder's take (OS-011c; here, in SDRAM, so that the bank itself, in SRAM_U, does not grow): where it begins (an index into the arena), how long it is, whether the
  // tail is held for it, whether it is live (ended and not yet committed).
  std::uint32_t take_start = 0;
  std::uint32_t take_frames = 0;
  bool take_held = false;
  bool take_live = false;
  std::uint32_t take_tail = 0;  // OS-LOOPER L2: the reserved tail behind the live take (even samples; the loop's undo buffer and its bitmap), 0 for every other take
  void (*clear_hook)(void*) noexcept = nullptr;  // OS-LOOPER: called by clear() and new_project() before anything is emptied (the recorder lets go of a loop that reads the arena, and stops a take that writes it)
  bool (*clear_busy)(void*) noexcept = nullptr;  // ... and true while the render has not yet let go: nothing writes the arena meanwhile (clear_busy())
  void* clear_self = nullptr;
  Step undo[kHistory];
  Step redo[kHistory];
  formats::wav::SampleReader reader;
  formats::FormatDesc wav_desc;
  formats::wt::SurgeReader wt_reader;  // OS-GRAN: Surge's .wt, read like a WAV (one load at a time: the file the bank has open)
  formats::FormatDesc wt_desc;
  formats::iff::SampleReader iff_reader;  // OS-SAMPFMT: AIFF / AIFF-C and IFF 8SVX (about 0.5 KB), the same way
  formats::FormatDesc iff_desc;
  StorageSource wav_source;
  TransientDetector::Onset onsets[TransientDetector::kMaxOnsets];
  TransientDetector::Onset scratch[TransientDetector::kMaxOnsets];  // choose_slices' working room
  SliceJob slice_job;          // the Chop page's AUTO slicer
  SampleEditJob edit_job;      // the Sample edit screen's effect
  PreviewJob preview_job;      // OS-FU2: the audition's reader (1.2 KB: the WAV importer)
  PtiLoadJob pti_job;          // OS-INSTUI S7: the LOAD of an instrument file (1.5 KB: the OG instrument reader)
  std::int16_t* preview_pcm = nullptr;  // OS-FU2: the audition's buffer (platform-given SDRAM; nullptr: no audition) and its size in samples
  std::uint32_t preview_cap = 0;
  const std::int16_t* over_pcm = nullptr;  // OS-REC2: the audio descriptor kPreviewSlot reads when preview_bind_over() bound it (nullptr: preview_bind()'s buffer, or none) ...
  std::uint32_t over_frames = 0;           // ... and its length
  std::int16_t* spare[SampleBankMem::kHistory + 1] = {};  // the edit pool's buffers and their capacities
  std::uint32_t spare_cap[SampleBankMem::kHistory + 1] = {};
  std::uint16_t auto_slices[kMaxSliceCount] = {};  // the slicer's answer, between the job and wave.auto_apply
  audio::StageCell stage;                           // OS-BUG-SLICEPITCH: the descriptor staged for the render (voices.h, Staged descriptor); about 0.3 KB, in SDRAM with the rest
  std::uint64_t reclaim_marks = 0;                  // OS-FU1: bit n: slot n has a stale block to reclaim at the next quiet handover
  std::uint32_t reclaims = 0;                       // blocks won back, for the tests and `diag`
  // OS-INSTUI S7: the clipboard (one instrument and a reference to its audio) and the swap records of the units that changed a slot's audio (sample_bank.h, THE CLIPBOARD AND SHARED AUDIO).
  static constexpr unsigned kSwaps = 16;
  struct Audio {
    std::int16_t* pcm = nullptr;
    std::uint32_t frames = 0;
    std::int16_t* side = nullptr;  // OS-STEREO (b): the audio's S, or nullptr
  };
  struct Swap {
    std::uint8_t slot = 0;
    bool valid = true;  // false: the mark of a unit whose swap was pushed out of the stack (undo / redo of it leaves the slot's audio alone, and the pairing of units and swaps holds)
    Audio before;
    Audio after;
  };
  model::Instrument clip_inst;
  Audio clip_audio;
  bool clip_valid = false;
  Swap swaps_done[kSwaps];
  Swap swaps_undone[kSwaps];
  unsigned swaps_done_n = 0;
  unsigned swaps_undone_n = 0;
};

class SampleBank {
 public:
  static constexpr unsigned kSlots = audio::kSampleSlots;
  static constexpr std::uint32_t kSettleMs = 5;
  // OS-BUG-SLICEPITCH: a staged descriptor the render has not taken after this long is given back (no render is running: the quiet handover writes it as before).
  static constexpr std::uint32_t kStageTimeoutMs = 50;
  // The longest audio (samples) the reclaim moves in one pass of the main loop: 256 KiB, about 10 ms of SDRAM copy; a longer one keeps its old block's room unused.
  static constexpr std::uint32_t kReclaimMax = 128u * 1024u;
  // A beat slice instrument's slice table: two 16-bit halves a boundary, up to kMaxSlices + 1 boundaries (og_samples.h slice_table_samples).
  static constexpr std::uint32_t kTableSamples = 2u * (audio::kMaxSlices + 1u);

  // The whole sample of a slot as it is in the arena; frames 0 when the slot has none.
  struct Pcm {
    const std::int16_t* data = nullptr;
    std::uint32_t frames = 0;
    const std::int16_t* side = nullptr;  // OS-STEREO (b): S, `frames` long, or nullptr (a mono sample)
  };

  SampleBank() = default;
  ~SampleBank() { close_wav(); }
  SampleBank(const SampleBank&) = delete;
  SampleBank& operator=(const SampleBank&) = delete;

  // ---- Setup (the platform, once; everything passed must outlive the bank) ----
  // `mem` is the bank's big parts (SampleBankMem: SDRAM on the device), `arena` the sample arena (`arena_samples` int16 values, 4-byte aligned), `peak_storage` the memory
  // the peak cache of the sample in view is built in (ui::plot::PeakCache::elements_required for the longest sample the screens draw; nullptr: no waveforms), `project` the
  // document's arena.
  void init(audio::VoicePool& voices, model::ModelArena& project, SampleBankMem& mem, std::int16_t* arena, std::uint32_t arena_samples,
            std::int16_t* peak_storage = nullptr, std::size_t peak_elems = 0) noexcept;
  // The slicer's hits (TransientDetector::kMaxOnsets), in the bank's SDRAM part.
  TransientDetector::Onset* onset_storage() noexcept { return mem_ != nullptr ? mem_->onsets : nullptr; }
  SliceJob& slice_job() noexcept { return mem_->slice_job; }
  SampleEditJob& edit_job() noexcept { return mem_->edit_job; }
  // The band-limited resampler's kernel table, the one the Amiga conversion builds (const: a cache; the standalone .pti save, OS-FILES-C C1b, builds it a slice at a time when it is not ready).
  AmigaKernel& resample_kernel() const noexcept { return mem_->edit_job.kernel(); }
  std::uint16_t* auto_slices() noexcept { return mem_->auto_slices; }
  TransientDetector::Onset* slicer_scratch() noexcept { return mem_->scratch; }
  void set_law(audio::LevelLaw law) noexcept {
    law_ = law;
    if (voices_ != nullptr) {
      voices_->set_law(law);  // OS-LIVEP: the live volume edit makes its gain with the project's law too
    }
  }
  audio::LevelLaw law() const noexcept { return law_; }
  // OS-BUG-PASTE24: the document's law (Project::volume_law) is the source; this brings the bank's copy, and the voices', to it. Called at the head of every rebuild, and writes only when the law
  // differs (a change happens at a load, with the engine quiet), so the render's read of VoicePool::law_ does not see a store at every Params edit.
  void sync_law() noexcept;
  // The demo kit is what the slots hold (install_demo_kit); clear() and any new audio in a slot end it. `diag inst` says so.
  bool demo_kit() const noexcept { return demo_kit_; }
  void set_demo_kit(bool on) noexcept { demo_kit_ = on; }

  // ---- The playback engine of a slot (OS-005h; voices.h SampleDesc::engine) ----
  // A slot's engine is its runtime override when one was set (set_engine; the tests and the console), else the one the instrument chose (model::Instrument::engine, stored in the
  // project since OS-004d: INST 1.2), else the default of an automatic instrument: AMIGA for a sample instrument that plays as one shot or a forward loop and whose recorded rate is a Paula rate (8,287 Hz,
  // the PAL rate of period 428, or 8,363 Hz, the NTSC one: what the OctaMED importer and a module sample's WAV carry), MODERN for every other. The rate is stored in the project
  // (INST 1.1 sample_rate), so the default survives a save and a reload. `set_amiga_by_rate(false)` turns the default off (the maintainer-reversible switch: every slot is Modern
  // unless overridden). A change rebinds the slots it touches (the handover of every rebind).
  static constexpr std::uint8_t kEngineAuto = 0xFF;
  void set_engine(unsigned slot, std::uint8_t engine) noexcept;
  std::uint8_t engine_override(unsigned slot) const noexcept { return slot < kSlots && engine_[slot] != 0u ? static_cast<std::uint8_t>(engine_[slot] - 1u) : kEngineAuto; }
  void set_amiga_by_rate(bool on) noexcept;
  bool amiga_by_rate() const noexcept { return amiga_by_rate_; }
  // The engine rebind_now() gives `in`'s slot.
  std::uint8_t engine_for(unsigned slot, const model::Instrument& in) const noexcept;
  bool ready() const noexcept { return voices_ != nullptr && arena_ != nullptr && mem_ != nullptr; }

  // ---- What the loaders placed ----
  // Every slot empty, the arena free, no rebind pending: a project load starts over (the caller has emptied the voices' table).
  void clear() noexcept;
  // OS-NEWKEEP (File > NEW): `keep_audio` false is clear(): every slot empty, the arena free. True (KEEP INSTRUMENTS): the audio and its slots stay where they are, but the new project has no
  // folder, so no slot's audio counts as held by a project folder any more (card_version() 0): its first save writes every sample into its own samples/ and refers to none of the old project's.
  void new_project(bool keep_audio) noexcept;
  // Slot `slot`'s whole sample is `frames` samples at `pcm` (inside the arena). The version moves. `block` (OS-ARENA-RECLAIM): the loader placed the sample in a block of its own of that many
  // samples (even, >= frames; the PCM and its padding, not the slice table that follows it): the slot owns it from then on, and a later load can reuse its room (RECLAIM). 0: not ours to move.
  // `side` (OS-STEREO (b)): the sample's side signal, which must be `pcm + even(frames)` (anything else is ignored), and `block` then covers both (>= 2 x even(frames), else the slot does not own the block).
  void adopt(unsigned slot, std::int16_t* pcm, std::uint32_t frames, std::uint32_t block = 0, std::int16_t* side = nullptr) noexcept;
  // The arena is in use up to `samples` (rounded up to even): the loaders' high-water mark. Never moves backward.
  void set_used(std::uint32_t samples) noexcept;

  // ---- Reading ----
  Pcm pcm(unsigned slot) const noexcept;
  // OS-STEREO step 2: whether the slot's audio also has a side signal S (decision S4 of docs/specs/OS-STEREO.md: an instrument plays stereo only with the flag AND S). The STEREO row of the source page asks it.
  bool has_side(unsigned slot) const noexcept { return mem_ != nullptr && slot < kSlots && mem_->slots[slot].pcm != nullptr && mem_->slots[slot].side != nullptr; }
  // Rule 4 of the risk review, the load half: every sample instrument of the document whose stereo flag is set and whose slot has no S gets the flag cleared (the document's own write, no undo unit: a load has
  // just made the document). Returns how many were cleared. The OgSampleLoader calls it when a project's samples are in.
  unsigned reconcile_stereo() noexcept;
  std::uint32_t version(unsigned slot) const noexcept { return mem_ != nullptr && slot < kSlots ? mem_->slots[slot].version : 0u; }
  // OS-FU2 (save_service.h, A PROJECT'S SAMPLES): the audio of `slot` was just read from the project's own folder (samples/<file>.wav), so that folder holds it as it is now:
  // a save into that folder has nothing to write for the slot until the audio changes (its version moves). card_version() is 0 for a slot with no such audio.
  // `size` / `stamp`: what the file looked like then (hal::storage::FileStamp; 0 / 0 when unknown, which a remount cannot confirm).
  void mark_on_card(unsigned slot, std::uint32_t size = 0, std::uint32_t stamp = 0) noexcept {
    if (mem_ != nullptr && slot < kSlots && mem_->slots[slot].pcm != nullptr) {
      mem_->slots[slot].card_version = mem_->slots[slot].version;
      mem_->slots[slot].card_size = size;
      mem_->slots[slot].card_stamp = stamp;
    }
  }
  std::uint32_t card_size(unsigned slot) const noexcept { return mem_ != nullptr && slot < kSlots ? mem_->slots[slot].card_size : 0u; }
  std::uint32_t card_stamp(unsigned slot) const noexcept { return mem_ != nullptr && slot < kSlots ? mem_->slots[slot].card_stamp : 0u; }
  std::uint32_t card_version(unsigned slot) const noexcept { return mem_ != nullptr && slot < kSlots ? mem_->slots[slot].card_version : 0u; }
  std::uint32_t free_samples() const noexcept { return arena_samples_ - used_; }
  std::uint32_t arena_samples() const noexcept { return arena_samples_; }
  const model::Project& project() const noexcept { return project_->project; }
  model::ModelArena& project_arena() noexcept { return *project_; }
  std::int16_t* peak_storage() const noexcept { return peak_storage_; }
  std::size_t peak_elems() const noexcept { return peak_elems_; }
  // The WAV sample reader as a registered format: what App::start_import wants (ImportRequest::format). One reader, one load at a time.
  const formats::FormatDesc& wav_format() const noexcept { return mem_->wav_desc; }
  const formats::FormatDesc& wt_format() const noexcept { return mem_->wt_desc; }  // OS-GRAN: the same for a .wt file
  const formats::FormatDesc& iff_format() const noexcept { return mem_->iff_desc; }  // OS-SAMPFMT: the same for AIFF / AIFF-C / 8SVX

  // ---- Loading a WAV from the card (the pieces App::start_import needs: the format above, a source, the audio's room) ----
  struct WavPlan {
    core::ByteSource* source = nullptr;
    std::int16_t* pcm = nullptr;      // the free tail of the arena: where the importer writes
    std::size_t pcm_capacity = 0;     // its size in samples
    std::uint32_t size = 0;           // the file's size in bytes
    // OS-SAMPFMT: the reader the file's first bytes call for (the header decides, never the extension): "RIFF" the WAV reader, "FORM" + AIFF / AIFC / 8SVX the IFF reader, "vawt" Surge's; nullptr when they
    // are none of those (the caller then goes by the name, and the reader it picks reports why the file is no good).
    const formats::FormatDesc* format = nullptr;
  };
  // Opens `path` (one file at a time: Busy while one is open). Ok fills `plan`; anything else is the storage's answer, or NoHandle when a file is already open or the arena
  // has no room left (free_samples() tells which).
  hal::storage::Status open_wav(hal::storage::Storage& storage, const char* path, WavPlan& plan) noexcept;
  // Closes the file (the import ended or was refused). Harmless when none is open.
  void close_wav() noexcept;
  bool wav_open() const noexcept { return wav_file_.is_open(); }

  // ---- Memory ----
  // The free tail of the arena for a sample whose length is not known before it is read (a WAV): its start (even index) and its size in samples; nullptr when
  // nothing is left. Nothing is taken until commit_load().
  std::int16_t* reserve(std::uint32_t& capacity) noexcept;
  // The sample read into reserve()'s memory is `frames` long and is slot `slot`'s now: the arena grows over it (even), the slot is adopted and a rebind is
  // requested. False (nothing changed) when `pcm` is not the tail, or `frames` is 0 or does not fit.
  // `side` (OS-STEREO (b)): the load has S as well, at `pcm + even(frames)`: both blocks are the slot's as one unit, the arena grows over 2 x even(frames) (False, nothing changed, when `side` is not there or they do not fit).
  bool commit_load(unsigned slot, std::int16_t* pcm, std::uint32_t frames, std::int16_t* side = nullptr) noexcept;
  // `samples` more from the tail (an edit's buffer, a slice table), even; nullptr when it does not fit.
  std::int16_t* alloc(std::uint32_t samples) noexcept;

  // ---- The recorder's take (OS-011c) ----
  // A recording is written by the render (audio/taps.h RecTap) straight into the arena's free tail, so the tail is HELD for it: take_begin() gives all of it (the arena is full
  // for every other taker, reserve / alloc / edit_begin / a slice table, until take_end()), because the take's length is not known before it ends. take_end(frames) then gives back
  // what the take did not use. The take lies in FRESH arena memory, never over audio a voice may read, and becomes a slot's audio only through take_commit(), which is the
  // loaders' commit_load() (the slot is adopted and a rebind requested: the handover above waits for stopped playback and idle live notes, so a recording made while the pattern
  // plays reaches the voices at the next safe point). A take that was not committed is given back when the recorder sees it thrown away (take_drop(): the next Recorder::poll in Idle, once no preview
  // note reads it) or, failing that, by the next take_begin(), while nothing was allocated after it; the room it took is otherwise the price of the arena being a bump allocator (the screens show
  // the free space).
  // Main loop only. clear() (a project load) forgets a held or live take: take_held() says so, the recorder gives the take up.
  std::int16_t* take_begin(std::uint32_t& capacity) noexcept;
  // The recording is `frames` samples long (0: nothing was recorded). Ends the hold.
  // OS-LOOPER L2: `tail` samples behind the take stay reserved with it (a loop's undo buffer and its bitmap, begun at take().data + even(frames)): the arena is used up to
  // start + even(frames) + even(tail), so no other taker lands there while the loop plays. 0 (every other take): as before. take_begin() gives both back when nothing was allocated
  // after the tail (DISCARD); take_commit() trims the tail when nothing was (SAVE); otherwise the tail stays where it is, `frames + bitmap` samples of arena lost until a project load
  // (the price of the bump allocator, as for any uncommitted take; nothing counts it, the screens' free-space figure shows it). The tail is clamped to the arena. Main loop only.
  void take_end(std::uint32_t frames, std::uint32_t tail = 0) noexcept;
  // The loop take's layout (arm() and the recorder's finish()): the bitmap of an undo buffer for `frames` frames is one bit per 64-frame block, in whole int16 samples, even.
  static constexpr std::uint32_t undo_bitmap_samples(std::uint32_t frames) noexcept {
    return ((((frames + 63u) / 64u + 15u) / 16u) + 1u) & ~1u;
  }
  // The longest loop (frames) whose take, undo buffer and bitmap fit `room` samples: the largest c with 2 x even(c) + undo_bitmap_samples(c) <= room (exactly what take_end(c, c + bitmap)
  // uses, so a take that stays within c never meets the arena's end).
  static std::uint32_t loop_capacity(std::uint32_t room) noexcept;
  bool take_held() const noexcept { return mem_ != nullptr && mem_->take_held; }
  // OS-RECCROP: the live take is cut down to [offset, offset + frames) (`offset` even; the recorder's crop()): the audio moves down to where the take began and the room behind it goes back to the arena
  // while nothing was allocated after the take. False (nothing moved) for no live take, a take with a reserved tail (a loop), or a window outside it. PRECONDITION: no note reads the take
  // (preview_quiet) and no render writes it. Main loop only; a memmove of the window (the cost of a crop).
  bool take_crop(std::uint32_t offset, std::uint32_t frames) noexcept;
  // OS-BUG-MEMDISCARD: the take that ended and was thrown away (DISCARD) is forgotten now, not at the next take_begin(): while nothing was allocated after it (and its tail) the arena is used up to
  // where it began again, so the free-space figure (the strip's MEM) gives the room back at once. Nothing if a take is held or none is live, and nothing while a note may still read the take:
  // the take pads and Shift + Preview play it through descriptor kPreviewSlot, so the drop waits for preview_quiet(live_idle) (`live_idle` as PreviewSink::live_idle says it). PRECONDITION: the
  // render no longer reads the take any other way (a loop under review has let go: the recorder asks loop().phase() first). Main loop only.
  void take_drop(bool live_idle = true) noexcept;
  // The last take that ended and was not committed: where it is and how long.
  // OS-LOOPER: `fn(self)` runs at the start of clear() and of new_project(): the recorder asks the render to let go of a loop that reads the arena and stops a take that writes it. `busy(self)` says the render
  // has not yet done so (a block, 1.5 ms): clear_busy() is true until it does, and every writer of the arena after a clear (OgSampleLoader::step) waits for it. One pair of hooks; main loop.
  void set_clear_hook(void (*fn)(void*) noexcept, bool (*busy)(void*) noexcept, void* self) noexcept {
    if (mem_ != nullptr) {
      mem_->clear_hook = fn;
      mem_->clear_busy = busy;
      mem_->clear_self = self;
    }
  }
  bool clear_busy() const noexcept { return mem_ != nullptr && mem_->clear_busy != nullptr && mem_->clear_busy(mem_->clear_self); }
  bool clear_hook_is(void* self) const noexcept { return mem_ != nullptr && mem_->clear_hook != nullptr && mem_->clear_self == self; }
  Pcm take() const noexcept { return mem_ != nullptr && mem_->take_live ? Pcm{arena_ + mem_->take_start, mem_->take_frames} : Pcm{}; }
  // The window [offset, offset + frames) of the take (offset rounded down to an even index: the audio is 4-byte aligned) is slot `slot`'s audio now: adopted, a rebind requested. False
  // (nothing changed) when there is no take, the window is empty or outside it. The take is the slot's from here on (the next take starts after it).
  bool take_commit(unsigned slot, std::uint32_t offset, std::uint32_t frames) noexcept;

  // ---- Sample edit's memory: a bounded history of versions of ONE slot's audio (OS-011a) ----
  // An edit is written into a buffer of its own (never over the audio the voices may read: the protocol of sample_edit_job.h), and committing it makes that buffer the
  // slot's audio and keeps the old one as an undo step: undo and redo move between versions by swapping the slot's pointer, no copying. The buffers are a pool of at
  // most kSpares, each as big as the sample was when the session began, taken from the arena's tail the first time they are needed and reused for ever after, so
  // a session of any length costs at most kSpares copies of the sample; a buffer is reused only when nothing references it: not a slot's audio, not what the
  // engine is bound to (it may still be playing it until the handover), not an undo or redo step. At most kHistory undo steps; an edit drops the redo ones.
  // A session belongs to one slot: editing another drops the history (the audio of both stays as it is). A project load clears everything.
  static constexpr unsigned kHistory = SampleBankMem::kHistory;
  static constexpr unsigned kSpares = kHistory + 1;
  // A buffer of `frames` for an edit's result, or nullptr when the arena has no room for one more copy and every buffer is in use.
  std::int16_t* edit_begin(unsigned slot, std::uint32_t frames) noexcept;
  // The edit written into `buf` (from edit_begin) is `frames` long and is the slot's audio now; `before` and `after` are the instrument's points and slices around it
  // (what undo and redo put back). The handover is requested. False (nothing changed) when `buf` is not a free buffer of the session.
  bool edit_commit(unsigned slot, std::int16_t* buf, std::uint32_t frames, const PointsRec& before, const PointsRec& after) noexcept;
  // One step back / forward: the slot's audio is the other version and `restore` the points to give the instrument. False when there is none.
  bool edit_undo(unsigned slot, PointsRec& restore) noexcept;
  bool edit_redo(unsigned slot, PointsRec& restore) noexcept;
  unsigned undo_depth(unsigned slot) const noexcept { return slot == hist_slot_ ? undo_n_ : 0u; }
  unsigned redo_depth(unsigned slot) const noexcept { return slot == hist_slot_ ? redo_n_ : 0u; }

  // ---- The clipboard and shared audio (OS-INSTUI S7; THE CLIPBOARD AND SHARED AUDIO above) ----
  // COPY: the clipboard takes `in` and a reference to `slot`'s audio (a sample instrument's; none for another kind). It replaces what was there. O6: a project load (clear()) empties it.
  void clip_set(const model::Instrument& in, unsigned slot) noexcept;
  bool clip_valid() const noexcept { return mem_ != nullptr && mem_->clip_valid; }
  const model::Instrument& clip_instrument() const noexcept { return mem_->clip_inst; }
  Pcm clip_pcm() const noexcept { return mem_ != nullptr && mem_->clip_valid ? Pcm{mem_->clip_audio.pcm, mem_->clip_audio.frames, mem_->clip_audio.side} : Pcm{}; }
  // PASTE, the audio half: `slot` reads the clipboard's block (a sample instrument on the clipboard; for another kind the slot's audio is left as it is, as a DELETE leaves it). Its version moves, the
  // handover is requested, the sample edit's history is dropped (it belongs to the audio that was replaced). A swap is recorded for every paste (one per undo unit); true when the audio changed.
  // False with no clipboard, or when the slot already read that block.
  bool paste_audio(unsigned slot) noexcept;
  // LOAD, the audio half: like commit_load(), and the swap is recorded, so that undoing the unit that put the instrument there gives the slot its earlier audio back.
  bool commit_load_undoable(unsigned slot, std::int16_t* pcm, std::uint32_t frames) noexcept;
  // The unit that paste_audio / commit_load_undoable belong to was undone / done again: `slot`'s audio is what it was before / after it. False when no swap of that slot is on record.
  bool audio_undone(unsigned slot) noexcept;
  bool audio_redone(unsigned slot) noexcept;
  // How many holders read the block starting at `p`: the slots (audio), the clipboard and the swap records. 0 for nullptr.
  unsigned pcm_refs(const std::int16_t* p) const noexcept;
  // A new unit discarded the journal's redo units (the dispatcher sees it): the swaps of the undone ones will never be asked for, and must not keep their blocks alive.
  void drop_undone_swaps() noexcept {
    if (mem_ != nullptr) {
      mem_->swaps_undone_n = 0;
    }
  }

  // ---- The audition (OS-FU2; Sample load's F5) ----
  // A buffer that is no instrument's: the first seconds of a WAV of the card, read as a job (PreviewJob, into preview_pcm), played by descriptor kPreviewSlot (voices.h), a
  // live note of `instrument` 65. It is not a slot of the arena (the buffer is the platform's, apart from it) and it does not touch the project, the slots' versions, the
  // peak cache or the history. THE HANDOVER'S RULE holds for it with its own window: the descriptor and the buffer are written only when no live note is queued or held
  // (`live_idle`, PreviewSink::live_idle, read first as in poll()) and no voice, sounding or fading, reads descriptor kPreviewSlot (VoicePool::slot_in_use): the sequencer
  // cannot start a note on it (a pattern has 64 instruments), so nothing else can. preview_quiet() says so; the App starts the read only when it holds and binds only when it
  // still holds, and a note that sounds is ended with the key's release before another read may begin.
  void set_preview_buffer(std::int16_t* pcm, std::uint32_t samples) noexcept {
    if (mem_ != nullptr) {
      mem_->preview_pcm = pcm;
      mem_->preview_cap = samples & ~1u;
    }
  }
  bool preview_ready() const noexcept { return voices_ != nullptr && mem_ != nullptr && mem_->preview_pcm != nullptr && mem_->preview_cap >= 2u; }
  std::int16_t* preview_pcm() const noexcept { return mem_ != nullptr ? mem_->preview_pcm : nullptr; }
  std::uint32_t preview_capacity() const noexcept { return mem_ != nullptr ? mem_->preview_cap : 0u; }
  PreviewJob& preview_job() noexcept { return mem_->preview_job; }
  PtiLoadJob& pti_job() noexcept { return mem_->pti_job; }
  bool preview_quiet(bool live_idle) const noexcept { return voices_ != nullptr && live_idle && !voices_->slot_in_use(audio::kPreviewSlot); }
  // The `frames` read into the buffer, recorded at `rate_hz`, become descriptor kPreviewSlot (one shot at the file's own pitch, unity gain, centre pan, Modern engine). False
  // (nothing written) when the quiet window is not there or `frames` is 0 or past the buffer. Main loop.
  bool preview_bind(std::uint32_t frames, std::uint32_t rate_hz, bool live_idle) noexcept;
  // The same over audio that already lies in the arena (OS-FU3: Shift + Preview from a frame, the Record take's preview): descriptor kPreviewSlot reads `frames` samples at `pcm`, nothing
  // is copied. The quiet window is the same; `pcm` must stay valid while the note sounds (the arena only grows, and a take stays until the next one is armed, which record.toggle refuses while the Preview key is down).
  bool preview_bind_over(const std::int16_t* pcm, std::uint32_t frames, std::uint32_t rate_hz, bool live_idle) noexcept;
  // Descriptor kPreviewSlot reads exactly these `frames` at `pcm` (preview_bind_over() bound them and nothing was bound since): the Record screen's pads (OS-REC2) play notes at once when
  // it does, and wait for the quiet window to bind when it does not (the take's trim moved, the audition or the Preview key bound another window).
  bool preview_is_over(const std::int16_t* pcm, std::uint32_t frames) const noexcept {
    return mem_ != nullptr && pcm != nullptr && mem_->over_pcm == pcm && mem_->over_frames == frames;
  }

  // ---- The handover (see above) ----
  // The engine is to play slot `slot` as the project's instrument and the slot's PCM now say. A slot with no sample instrument, or no PCM, is made silent.
  void request_rebind(unsigned slot) noexcept;
  // The same mark for an edit whose only difference from the engine's descriptor is a loop move the render has been sent (OS-MOD, LIVE LOOP): the slot is not `stale`.
  void request_rebind_loop(unsigned slot) noexcept;
  // Slot `slot` waits for a handover that changes more than its loop points (the window, the mode, the PCM, an undo): its descriptor's window is not the project's, so a live
  // loop move, counted from the project's window start, would land in the wrong place (OS-MOD review): the Wave screen sends none until the handover.
  bool stale(unsigned slot) const noexcept { return slot < kSlots && ((stale_ >> slot) & 1u) != 0u; }
  // A slot whose descriptor the render has taken from the staging cell (STAGED DESCRIPTOR below) is not pending for the sound any more: its handover only moves the bookkeeping (the old audio's release, RECLAIM).
  bool rebind_pending() const noexcept { return (pending_ & ~staged_) != 0u; }
  bool pending(unsigned slot) const noexcept { return slot < kSlots && ((pending_ >> slot) & 1u) != 0u; }
  // One pass of the main loop: `now_ms` is the 64-bit uptime, `stopped` says no sequenced note can start (the transport is stopped), `live_idle` that no live note is queued or
  // held (it must be read BEFORE this call in the same pass: see THE HANDOVER). True when the quiet handover wrote descriptors (a staged one is not counted). Nothing is applied while a WAV is being read (a slice
  // table would take the tail the import is writing into). `jobs_idle` (no job is running: none reads or writes a sample) lets the reclaim of a replaced block move audio (RECLAIM above).
  // `params_idle`: the render's ParamChange ring is empty (the staged descriptor is built from the project, so no change that names the slot may still be queued). It is also part of `live_idle` as the App passes it.
  bool poll(std::uint64_t now_ms, bool stopped, bool live_idle = true, bool jobs_idle = true, bool params_idle = true) noexcept;
  // Writes the descriptor of `slot` now, with no wait: only for a caller that has made the engine quiet itself (the project loaders do) and for the tests.
  void rebind_now(unsigned slot) noexcept;
  std::uint32_t rebinds() const noexcept { return rebinds_; }  // descriptors written, for the diagnostics and the tests
  std::uint32_t reclaims() const noexcept { return mem_ != nullptr ? mem_->reclaims : 0u; }  // replaced blocks whose room was won back (RECLAIM)

 private:
  void forget_take() noexcept;  // take_drop() without the preview check (take_begin(): arm() has made sure no note reads the take)
  audio::VoicePool* voices_ = nullptr;
  model::ModelArena* project_ = nullptr;
  std::int16_t* arena_ = nullptr;
  std::uint32_t arena_samples_ = 0;
  std::uint32_t used_ = 0;
  std::int16_t* peak_storage_ = nullptr;
  std::size_t peak_elems_ = 0;
  audio::LevelLaw law_ = audio::LevelLaw::Og12;
  bool demo_kit_ = false;  // OS-BUG-PASTE24: the slots hold the demo kit (set_demo_kit); clear(), adopt() and set_audio() (another sound in a slot) end it
  SampleBankMem* mem_ = nullptr;
  std::uint64_t pending_ = 0;       // bit n: slot n waits for the handover
  std::uint64_t stale_ = 0;         // bit n: ... for more than a loop move (stale(); always a subset of pending_)
  // STAGED DESCRIPTOR (OS-BUG-SLICEPITCH; voices.h). poll() hands the render the descriptor of a marked slot without waiting for silence: one slot at a time through the one cell. The slot stays pending_
  // (the quiet handover still rewrites the same descriptor, moves `bound` and reclaims), and `bound` keeps naming the OLD audio until then, so the old block stays protected for any voice that still
  // reads it (Amiga voices cache the pointer). todo_: bit n wants a stage (set by request_rebind); staged_: the render has taken slot n's descriptor (cleared by request_rebind and the quiet handover).
  std::uint64_t todo_ = 0;
  std::uint64_t staged_ = 0;
  unsigned staging_ = kSlots;             // the slot in the cell (kSlots: none, or forgotten because the slot was marked again)
  std::uint32_t staging_takes_ = 0;       // VoicePool::stage_takes() when it was staged
  bool busy_seen_ = false;                // the cell was busy at the last poll ...
  std::uint64_t busy_since_ = 0;          // ... since this uptime
  bool stage_step(std::uint64_t now_ms, bool params_idle) noexcept;  // true when a descriptor was staged
  std::uint8_t engine_[kSlots] = {};  // OS-005h: the slots' engine overrides + 1 (0: the instrument's default)
  bool amiga_by_rate_ = true;       // the default engine of an instrument at a Paula rate is Amiga
  bool quiet_ = false;              // the engine was quiet at the last poll ...
  std::uint64_t quiet_since_ = 0;   // ... since this uptime
  std::uint32_t rebinds_ = 0;
  bool alive(const std::int16_t* p) const noexcept;
  void drop_oldest_undo() noexcept;
  void set_audio(unsigned slot, std::int16_t* pcm, std::uint32_t frames, std::int16_t* side = nullptr) noexcept;
  bool reclaim(unsigned slot) noexcept;  // RECLAIM: true when the stale block's room was won back (and the slot rebound), the mark done with
  bool block_in_history(const std::int16_t* from, const std::int16_t* to) const noexcept;
  // Does anything but slot `skip` (or any holder when `skip` is kSlots) read from [from, to)? (a slot's audio or bound descriptor, the clipboard, a swap record): RECLAIM must not move or overwrite it.
  bool referenced(const std::int16_t* from, const std::int16_t* to, unsigned skip) const noexcept;
  void set_slot_audio(unsigned slot, const SampleBankMem::Audio& a) noexcept;
  void push_swap(unsigned slot, const SampleBankMem::Audio& before, const SampleBankMem::Audio& after) noexcept;

  unsigned hist_slot_ = kSlots;  // the slot whose history this is (kSlots: none)
  unsigned undo_n_ = 0;
  unsigned redo_n_ = 0;
  unsigned spare_n_ = 0;
  hal::storage::Storage* wav_storage_ = nullptr;
  hal::storage::FileHandle wav_file_{};
};

// OS-BUG-PASTE24: the demo kit into the start of the arena and the voices' table, its four instruments into `project` (at the level that is 0 dB under the document's law: the kit is bound at unity,
// so a rebuild from the project must give the same gain) and, if pattern 1 is empty, its pattern. Only into an empty voices table and a project the kit fits in (demo_kit_fits).
// True when it was installed. This is the platform's
// install_demo, here so that the host can hold it to the rebuild invariant.
// OS-BUG-DEMOKIT: the kit's four instruments go into slots 0..3, so it fits only a project that holds no sample or module instrument (it would have sounds of its own) and nothing
// at all in slots 0..47 (a project of module or MIDI instruments lost them to the kit). Only MIDI instruments in slots 48..63 do not stop it: every OG project holds the 16 MIDI channels there.
bool demo_kit_fits(const model::Project& project) noexcept;

bool install_demo_kit(SampleBank& bank, audio::VoicePool& voices, model::Project& project, std::int16_t* arena, std::uint32_t arena_samples) noexcept;

}  // namespace o3os::app
