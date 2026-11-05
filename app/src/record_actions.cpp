// SPDX-License-Identifier: GPL-3.0-or-later
// The Record screen's actions (OS-011c): what the keys do to the recorder (recorder.h has the rules and their sources). Nothing here is undoable: the recorder's state is
// runtime, and the one thing that reaches the project, SAVE (an instrument made of the take), ends the undo history as an import does (it changes the project behind the
// editor's back, through the same model call the WAV importer uses).
#include "action_handlers.h"

#include "sample_text.h"

#include <o3os/app/key_prompt.h>
#include <o3os/app/og_samples.h>
#include <o3os/app/record_view.h>
#include <o3os/app/recorder.h>
#include <o3os/app/sample_bank.h>
#include <o3os/formats/wav_sample.h>

#include <cstring>

namespace o3os::app {

namespace {

constexpr int kCoarseGain = 5;     // a Shift click of the gain is five percent
constexpr int kCoarseFreq = 10;    // ... of the frequency ten channels
constexpr int kCoarseTap = 4;
constexpr int kCoarseThreshold = 3;  // ... of THRESHOLD nine dB
constexpr int kCoarseStop = 4;    // ... of STOP AT four choices (a bar count moves by four bars)
constexpr int kCoarseCountIn = 2;  // ... of COUNT-IN two choices (OFF, 2 BARS, 4 BARS)
constexpr std::uint32_t kCoarseTrim = 16;  // ... of a trim marker sixteen overview slots

bool on_record(const ActionContext& ctx) noexcept { return ctx.view.screen == ScreenId::Record && ctx.rec != nullptr && ctx.rec->ready(); }

bool say(ActionContext& ctx, core::Severity severity, const char* text) noexcept {
  (void)ctx.view.notify.upsert(severity, text, "record", ViewState::kMessageMs);
  return true;
}

// OS-QAFIX-E-SREC-09: a take under review is held in the arena only; nothing undoes the loss of it, so the keys that would lose its audio ask first (the strip says so, the key row marks the key: key_prompt.cpp).
// True: go on, the same key asked within TakeAsk::kWindowMs and its question is still the head of the strip (a question of an earlier take, left behind by a SAVE or a message, never answers: take_question() marks the key by
// the same test). False: the question is on the strip now.
bool confirmed(ActionContext& ctx, TakeAsk::Kind kind, Ask which, const char* head) noexcept {
  TakeAsk& a = ctx.view.take_ask;
  if (a.kind == kind && core::tick_elapsed(ctx.now, a.at) <= core::ticks_from_ms(TakeAsk::kWindowMs) && ask_is_up(ctx.view, which, head)) {
    a.kind = TakeAsk::Kind::None;
    return true;
  }
  a.kind = kind;
  a.at = ctx.now;
  AskText text;
  ask_text(which, head, text);
  (void)ctx.view.notify.ask(core::Severity::Info, text.c_str(), "record", TakeAsk::kWindowMs);
  return false;
}

// The COUNT-IN setting in bars (0, 1 or 2) when the transport is stopped and can be started from here, else 0.
unsigned count_in_bars(const ActionContext& ctx) noexcept {
  if (ctx.view.play.playing || ctx.transport == nullptr || ctx.model.device.kind() != core::ParamKind::Device) {
    return 0;
  }
  const float v = ctx.model.device.get(model::device_setting::CountIn);
  return v >= 1.5f ? 2u : (v >= 0.5f ? 1u : 0u);
}

bool live(const Recorder& rec) noexcept { return rec.state() == Recorder::State::Armed || rec.state() == Recorder::State::Recording; }

RecRow row_under_cursor(const ActionContext& ctx) noexcept {
  const RecMode mode = rec_mode_of_page(ctx.view.page[screen_index(ScreenId::Record)]);
  const bool review = ctx.rec->state() == Recorder::State::Review;
  const bool looped = ctx.rec->loop_review();
  const bool level = level_rows(ctx.rec->cfg);
  return rec_rows(mode, review, looped, level).row[rec_cursor(ctx.view.record, mode, review, looped, level)];
}

bool edit(ActionContext& ctx, int detents, bool coarse) noexcept {
  if (!on_record(ctx) || detents == 0) {
    return false;
  }
  Recorder& rec = *ctx.rec;
  const bool frozen = live(rec);  // while a take is armed or recording the source, the tap and the start cannot change under it
  const bool locked = frozen || rec.loop_review();  // OS-LOOPER: and the source of a loop is the one it was recorded from for as long as the loop exists (the overdub hears it)
  const std::uint32_t unit = rec.overview().slot_frames() * (coarse ? kCoarseTrim : 1u);
  switch (row_under_cursor(ctx)) {
    case RecRow::Source:
      if (locked) return false;
      rec.step_source(detents);
      return true;
    case RecRow::Gain:
      rec.step_gain(coarse ? detents * kCoarseGain : detents);
      return true;
    case RecRow::Monitor:
      rec.toggle_monitor();
      return true;
    case RecRow::Align:
      if (frozen) return false;
      if (rec.cfg.loop_bars != 0u) return say(ctx, core::Severity::Info, "LOOP STARTS THE TAKE ON A BAR");
      rec.step_align(detents > 0 ? 1 : -1);
      return true;
    case RecRow::Threshold:  // OS-SREC-LEVEL: read when the take is armed, so a take that listens cannot change it
      if (frozen) return false;
      rec.step_threshold(coarse ? detents * kCoarseThreshold : detents);
      return true;
    case RecRow::PreRoll:
      if (frozen) return false;
      rec.step_preroll(detents > 0 ? 1 : -1);
      return true;
    case RecRow::StopAt:
      if (frozen) return false;
      if (rec.cfg.loop_bars != 0u) return say(ctx, core::Severity::Info, "LOOP ENDS THE TAKE AFTER ITS BARS");
      rec.step_stop(coarse ? detents * kCoarseStop : detents);
      return true;
    case RecRow::Loop:
      if (frozen) return false;
      rec.step_loop(coarse ? detents * kCoarseStop : detents);
      return true;
    case RecRow::CountIn:  // OS-RECCOUNT: read at F1, like LOOP: frozen while a take runs
      if (frozen) return false;
      rec.step_count_in(coarse ? detents * kCoarseCountIn : detents);
      return true;
    case RecRow::LoopVol:
      rec.step_loop_vol(coarse ? detents * kCoarseGain : detents);
      return true;
    case RecRow::Feedback:
      rec.step_loop_fb(coarse ? detents * kCoarseGain : detents);
      return true;
    case RecRow::Tap:
      if (locked) return false;
      rec.step_tap(coarse ? detents * kCoarseTap : detents);
      return true;
    case RecRow::Freq:
      if (locked) return false;
      rec.radio_step(coarse ? detents * kCoarseFreq : detents);
      return true;
    case RecRow::Region:
      if (locked) return false;
      rec.radio_region_step(detents > 0 ? 1 : -1);
      return true;
    case RecRow::TrimStart:
      rec.step_trim_start(detents, unit);
      return true;
    case RecRow::TrimEnd:
      rec.step_trim_end(detents, unit);
      return true;
  }
  return false;
}

}  // namespace

bool record_edit(ActionContext& ctx, core::ActionArg arg) noexcept { return edit(ctx, static_cast<int>(arg), false); }
bool record_coarse(ActionContext& ctx, core::ActionArg arg) noexcept { return edit(ctx, static_cast<int>(arg), true); }

// Delete: the row's default (our default: line in, gain 0, the monitor in its own default, 87.5 MHz).
bool record_reset(ActionContext& ctx, core::ActionArg) noexcept {
  if (!on_record(ctx)) {
    return false;
  }
  Recorder& rec = *ctx.rec;
  const bool frozen = live(rec);
  const bool locked = frozen || rec.loop_review();
  switch (row_under_cursor(ctx)) {
    case RecRow::Source:
      if (locked) return false;
      rec.step_source(-100);
      return true;
    case RecRow::Gain:
      rec.reset_gain();
      return true;
    case RecRow::Monitor:
      if (rec.cfg.monitor) {
        rec.toggle_monitor();
      }
      return true;
    case RecRow::Align:
      if (frozen) return false;
      if (rec.cfg.loop_bars != 0u) return say(ctx, core::Severity::Info, "LOOP STARTS THE TAKE ON A BAR");
      rec.step_align(-100);
      rec.step_align(static_cast<int>(RecAlign::Bar));
      return true;
    case RecRow::Threshold:
      if (frozen) return false;
      rec.step_threshold(-100);
      rec.step_threshold((kRecLevelDefaultDb - kRecLevelMinDb) / kRecLevelStepDb);
      return true;
    case RecRow::PreRoll:
      if (frozen) return false;
      rec.step_preroll(-100);
      rec.step_preroll(static_cast<int>(kRecPreRollDefault));
      return true;
    case RecRow::StopAt:
      if (frozen) return false;
      if (rec.cfg.loop_bars != 0u) return say(ctx, core::Severity::Info, "LOOP ENDS THE TAKE AFTER ITS BARS");
      rec.step_stop(-100);  // MANUAL: today's behaviour
      return true;
    case RecRow::Loop:
      if (frozen) return false;
      rec.step_loop(-100);  // OFF
      return true;
    case RecRow::CountIn:  // OS-RECCOUNT: OFF
      if (frozen) return false;
      rec.step_count_in(-100);
      return true;
    case RecRow::LoopVol:
      rec.step_loop_vol(100);  // unity
      return true;
    case RecRow::Feedback:
      rec.step_loop_fb(100);  // the layer adds
      return true;
    case RecRow::Tap:
      if (locked) return false;
      rec.step_tap(-100);
      return true;
    case RecRow::Freq:
      if (locked) return false;
      rec.radio_set(8750);
      return true;
    case RecRow::Region:
      if (locked) return false;
      rec.radio_region_step(-100);
      return true;
    case RecRow::TrimStart:
      rec.set_trim_start(0);
      return true;
    case RecRow::TrimEnd:
      rec.set_trim_end(rec.frames());
      return true;
  }
  return false;
}

// F1 and Rec: arm the take (it starts now or on the step START ON says), or stop the one that is armed or running (at once, or at the boundary STOP AT says: Recorder::stop).
bool record_toggle(ActionContext& ctx, core::ActionArg) noexcept {
  if (!on_record(ctx)) {
    return false;
  }
  Recorder& rec = *ctx.rec;
  if (live(rec)) {
    rec.stop();
    return true;
  }
  if (rec.loop_review()) {  // OS-LOOPER L2c: F1 under a loop's review is the overdub, on or off (spec 3c.3)
    switch (rec.overdub_toggle()) {
      case Recorder::Overdub::Started:
        if (!ctx.view.play.playing && ctx.transport != nullptr) {  // Q8: stopped, the count-in (or Play) starts the transport and the session begins with the loop's first bar
          const unsigned bars = count_in_bars(ctx);
          const bool ok = bars != 0u ? ctx.transport->play_count_in(ctx.view.edit.pattern, bars) : ctx.transport->play(ctx.view.edit.pattern);
          if (!ok) {
            (void)rec.overdub_toggle();  // punch out again: the transport did not start
            return say(ctx, core::Severity::Warn, bars != 0u ? "THE COUNT-IN DID NOT START" : "THE TRANSPORT DID NOT START");
          }
          ctx.view.playing = true;
        }
        return true;
      case Recorder::Overdub::Stopped:
        return true;
      case Recorder::Overdub::Undoing:
        return say(ctx, core::Severity::Info, "UNDO IN PROGRESS");
      case Recorder::Overdub::Ignored:
      case Recorder::Overdub::NoLoop:
        return false;
    }
    return false;
  }
  if (ctx.view.hold.button != HeldPreview::kNone) {
    return false;  // the Preview key is down: it plays the take's memory (OS-FU3)
  }
  // The pads play the take's memory too (OS-REC2): a new take is written over it from the next block, so none may be down, and no voice may still be reading it (the release of a
  // note is a few milliseconds: preview_quiet() is the bank's own "no live note held or queued, no voice on the preview descriptor").
  if (ctx.view.take_pads.any() || (ctx.samples != nullptr && !ctx.samples->preview_quiet(ctx.preview == nullptr || ctx.preview->live_idle()))) {
    return say(ctx, core::Severity::Info, "THE TAKE IS STILL PLAYING");
  }
  if (rec.state() == Recorder::State::Review && !rec.take_saved() && !confirmed(ctx, TakeAsk::Kind::Record, Ask::TakeRec, kAskHeadTake)) {
    return true;  // E-SREC-09: a new take is written over the one under review, which nothing has saved: it asks first (F1 is the key that stops a take: a double press must not wipe it)
  }
  // OS-METRO: stopped, with an aligned take (ALIGN is not FREE) and COUNT-IN on, the take waits for the start point and the transport counts in the pattern being edited first (the Play key's pattern):
  // the take begins at the first step after the count-in, to the sample. Otherwise an aligned take falls back to FREE while the transport is stopped, as before.
  // OS-SREC-LEVEL: LEVEL needs no timeline (the take listens for the level whether the transport runs or not), so it never counts in; only a LOOP, which is on the bars, does.
  // OS-LOOPER: a loop take is on the bars, so it needs the transport: stopped, F1 starts it (after the count-in when COUNT-IN is on; with it off the transport and the take start together).
  // OS-RECCOUNT: the Record card's own COUNT-IN row applies to a bar-aligned take only (LOOP on, or START ON = BAR) and replaces the Config COUNT-IN for that take: from stop F1 starts the
  // transport AT ONCE with plain play (not play_count_in) and the recorder counts the N bars in itself, recording from the first step of bar N+1; with the transport running the take waits
  // for the next bar boundary, then N more bars. Never to FREE, STEP, PATTERN, LEVEL, and never to the OVERDUB above (QA-REC-032 keeps the Config count-in there).
  const bool looping = rec.cfg.loop_bars != 0u;
  if (looping && !ctx.view.play.playing && ctx.transport == nullptr) {
    return say(ctx, core::Severity::Warn, "THE LOOP NEEDS THE TRANSPORT");
  }
  const bool was_playing = ctx.view.play.playing;
  const unsigned row_ci = count_in_applies(rec.cfg) && (was_playing || ctx.transport != nullptr) ? rec.cfg.count_in : 0u;
  unsigned bars = 0;
  if (!was_playing && ((rec.cfg.align != RecAlign::Free && rec.cfg.align != RecAlign::Level) || looping) && ctx.transport != nullptr && ctx.model.device.kind() == core::ParamKind::Device) {
    const float v = ctx.model.device.get(model::device_setting::CountIn);
    bars = v >= 1.5f ? 2u : (v >= 0.5f ? 1u : 0u);
  }
  if (row_ci != 0u) {
    bars = 0;  // the row replaces the Config COUNT-IN for this take
  }
  const bool start_transport = (looping || row_ci != 0u) && !was_playing && bars == 0u;
  // The tempo the loop is made at: the sequencer's while it plays, else the project's (the arena must hold its bars; the header says when the tempo moves later).
  const std::uint32_t tempo = !looping ? 0u : (was_playing && ctx.view.play.tempo_x100 != 0u ? ctx.view.play.tempo_x100 : (ctx.model.project != nullptr ? ctx.model.project->tempo_x100() : 0u));
  const bool arm_playing = was_playing || bars != 0u || start_transport;
  const Recorder::ArmResult result = row_ci != 0u ? rec.arm_count_in(arm_playing, row_ci, ctx.view.play.step, ctx.view.play.pattern, !was_playing, tempo) : rec.arm(arm_playing, tempo);
  switch (result) {
    case Recorder::ArmResult::Ok:
      if (bars != 0u) {
        if (!ctx.transport->play_count_in(ctx.view.edit.pattern, bars)) {
          rec.stop();  // the transport did not start: the take that waits for it would wait for ever
          return say(ctx, core::Severity::Warn, "THE COUNT-IN DID NOT START");
        }
        ctx.view.playing = true;
      } else if (start_transport) {
        if (!ctx.transport->play(ctx.view.edit.pattern)) {  // OS-RECCOUNT: plain play, NOT the Config count-in: the row's own count-in runs in the recorder
          rec.stop();
          return say(ctx, core::Severity::Warn, "THE TRANSPORT DID NOT START");
        }
        ctx.view.playing = true;
      }
      return true;
    case Recorder::ArmResult::Settling:
      return say(ctx, core::Severity::Info, "THE SOURCE IS SETTLING: TRY AGAIN");
    case Recorder::ArmResult::NoRoom:
      return say(ctx, core::Severity::Warn, looping ? "NO ROOM IN THE SAMPLE MEMORY FOR THE LOOP" : "NO ROOM IN THE SAMPLE MEMORY");
    case Recorder::ArmResult::NotReady:
      return say(ctx, core::Severity::Warn, "THE INPUT IS NOT READY");
    case Recorder::ArmResult::Busy:
      return say(ctx, core::Severity::Info, "A TAKE IS RUNNING");
  }
  return false;
}

// A pad of the Record screen (OS-REC2; record_view.h has the layout): while a take is under review the pad plays its trim window at the pitch of the pad's
// note, through the preview descriptor (App::poll_take_pads binds it over the take and starts the note; the pad's release ends it). arg = the pad 0..47.
bool record_pad(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (!on_record(ctx) || arg < 0 || arg >= static_cast<core::ActionArg>(kRecPads)) {
    return false;
  }
  const Recorder& rec = *ctx.rec;
  if (rec.state() != Recorder::State::Review || rec.take_data() == nullptr || rec.trim_end() <= rec.trim_start()) {
    return say(ctx, core::Severity::Info, "NO TAKE TO PLAY: F1 RECORDS ONE");
  }
  const unsigned pad = static_cast<unsigned>(arg);
  const std::uint64_t bit = std::uint64_t{1} << pad;
  const std::int16_t* pcm = rec.take_data() + rec.trim_start();
  const std::uint32_t frames = rec.trim_end() - rec.trim_start();
  if (ctx.preview != nullptr && ctx.samples != nullptr && ctx.samples->preview_is_over(pcm, frames)) {
    ctx.preview->take_pad_down(pad, static_cast<unsigned>(ctx.view.pattern.track()), rec_pad_note(pad));  // the descriptor is already the take's window: sound at once
    ctx.view.take_pads.down |= bit;
  } else {
    ctx.view.take_pads.pending |= bit;  // the descriptor must be bound first, in the quiet window: App::poll_take_pads
  }
  return false;  // nothing on screen changes by the press itself (the playhead is drawn from the voice)
}

// F2: the monitor (the input heard through the engine). Not on RESAMPLE: what the engine plays is heard already.
bool record_monitor(ActionContext& ctx, core::ActionArg) noexcept {
  if (!on_record(ctx) || rec_mode_of_page(ctx.view.page[screen_index(ScreenId::Record)]) == RecMode::Resample) {
    return false;
  }
  ctx.rec->toggle_monitor();
  return true;
}

// F4: the take under review is thrown away. OS-SILENTMSG: with nothing recorded (no take yet, or one only armed) the key says so; a take being written is left to F1 (silent).
bool record_discard(ActionContext& ctx, core::ActionArg) noexcept {
  if (!on_record(ctx)) {
    return false;
  }
  if (ctx.rec->state() == Recorder::State::Idle || ctx.rec->state() == Recorder::State::Armed) {
    return say(ctx, core::Severity::Info, "NOTHING RECORDED");
  }
  if (ctx.rec->state() != Recorder::State::Review || ctx.rec->save_waiting()) {
    return false;
  }
  if (!ctx.rec->take_saved() && !confirmed(ctx, TakeAsk::Kind::Discard, Ask::TakeDrop, kAskHeadTake)) {
    return true;  // E-SREC-09: the take was not saved (F3 SAVE ends the review, F7 SAVE WAV marks it): it asks first
  }
  ctx.rec->discard();
  return say(ctx, core::Severity::Info, "TAKE DISCARDED");
}

// Shift + F3: the take becomes its window (OS-RECCROP, finding 25). The audio outside TRIM START and TRIM END is gone, so it asks first; nothing to cut and a loop say so. A note that still reads the take
// (the pads play it) holds the crop back, as it does a new take.
bool record_crop(ActionContext& ctx, core::ActionArg) noexcept {
  if (!on_record(ctx)) {
    return false;
  }
  Recorder& rec = *ctx.rec;
  if (rec.save_waiting()) {
    return false;
  }
  if (rec.state() != Recorder::State::Review) {
    return say(ctx, core::Severity::Info, "NO TAKE TO CROP");
  }
  if (rec.loop_review()) {
    return say(ctx, core::Severity::Info, "A LOOP IS NOT CROPPED");
  }
  if (rec.trim_start() == 0u && rec.trim_end() == rec.frames()) {
    return say(ctx, core::Severity::Info, "NOTHING TO CROP: MOVE TRIM START OR END");
  }
  if (ctx.view.take_pads.any() || (ctx.samples != nullptr && !ctx.samples->preview_quiet(ctx.preview == nullptr || ctx.preview->live_idle()))) {
    return say(ctx, core::Severity::Info, "THE TAKE IS STILL PLAYING");
  }
  if (!confirmed(ctx, TakeAsk::Kind::Crop, Ask::TakeCrop, kAskHeadCrop)) {
    return true;
  }
  return rec.crop() ? say(ctx, core::Severity::Info, "TAKE CROPPED") : say(ctx, core::Severity::Warn, "THE TAKE COULD NOT BE CROPPED");
}

// F5 / F6 on the RADIO page: seek down / up (the chip stops on the next station, wrapping at the band's end). OS-SHIFTROW: an argument of 10 or more (Shift + F5 / F6, CH-10 / CH+10) is
// ten channels of the region's grid at a time, tuned straight (a long way across the band without waiting for the chip to find each station).
bool record_seek(ActionContext& ctx, core::ActionArg arg) noexcept {
  if (on_record(ctx) && ctx.rec->loop_review()) {  // OS-LOOPER L2c: under a loop's review F5 is UNDO LAYER (F6 and the radio's keys do nothing: the loop's source is locked)
    if (arg != -1 || ctx.rec->save_waiting()) {
      return false;
    }
    return ctx.rec->undo_layer() ? true : say(ctx, core::Severity::Info, "NO LAYER TO UNDO");
  }
  if (!on_record(ctx) || rec_mode_of_page(ctx.view.page[screen_index(ScreenId::Record)]) != RecMode::Radio) {
    return false;
  }
  if (arg >= 10 || arg <= -10) {
    const std::uint16_t before = ctx.rec->cfg.radio_freq;
    ctx.rec->radio_step(static_cast<int>(arg));
    return ctx.rec->cfg.radio_freq != before;
  }
  ctx.rec->radio_seek(arg > 0);
  return true;
}

// F3: the take's window (the trim) becomes the audio of the first free instrument slot.
// The sample bank takes the take's own memory (SampleBank::take_commit) and the voices get it at the next safe point; the instrument is made as a WAV import makes one.
bool record_save(ActionContext& ctx, core::ActionArg) noexcept {
  if (!on_record(ctx)) {
    return false;
  }
  Recorder& rec = *ctx.rec;
  if (rec.save_waiting() && !rec.save_ready()) {
    return false;  // SaveWait (spec 3c.2): the screen keeps drawing the review and the keys are ignored until the render has acknowledged the end of the session
  }
  if (rec.state() != Recorder::State::Review) {
    return say(ctx, core::Severity::Info, "NO TAKE TO SAVE: F1 RECORDS ONE");
  }
  if (ctx.samples == nullptr || !ctx.samples->ready()) {
    return say(ctx, core::Severity::Warn, "NO SAMPLE MEMORY");
  }
  model::Project& project = ctx.samples->project_arena().project;
  unsigned slot = SampleBank::kSlots;
  for (unsigned i = 0; i < SampleBank::kSlots; ++i) {
    if (project.instrument(i) == nullptr) {
      slot = i;
      break;
    }
  }
  if (slot == SampleBank::kSlots) {
    return say(ctx, core::Severity::Warn, "NO FREE INSTRUMENT SLOT");
  }
  if (!rec.save_waiting() && rec.loop_review()) {  // OS-LOOPER L2c: SAVE is two steps while a session is on: quiesce first, commit when the render has acknowledged (App::poll_recorder runs this again)
    if (rec.undo_busy()) {
      return say(ctx, core::Severity::Info, "UNDO IN PROGRESS");
    }
    if (rec.needs_quiesce()) {
      rec.begin_save();
      return true;
    }
  }
  const std::uint32_t frames = rec.trim_end() - rec.trim_start();
  const unsigned loop_bars = rec.loop_review() ? rec.loop_bars() : 0u;  // OS-LOOPER: a loop is saved whole (no trim) as an instrument that loops
  if (!rec.commit(slot)) {
    return say(ctx, core::Severity::Warn, "THE TAKE COULD NOT BE SAVED");
  }
  formats::wav::Info info;
  info.sample_rate = 0;  // 0: 44,100 Hz, the rate of an OG folder's files; the engine plays at its own rate, nothing is resampled
  info.frames = frames;
  info.channels = 1;
  char name[16] = "RECORDING";
  if (loop_bars != 0u) {  // play mode LOOP FWD over the whole sample: exactly N bars, so a note plays the loop in a loop
    info.has_loop = true;
    info.loop_type = formats::wav::LoopType::Forward;
    info.loop_start = 0;
    info.loop_end = frames;
    core::FixedString<16> loop_name;
    loop_name.assign("LOOP ");
    append_uint(loop_name, loop_bars);
    loop_name.append("B");
    std::memcpy(name, loop_name.c_str(), loop_name.size() + 1u);
  }
  model::Instrument inst;
  formats::wav::make_sample_instrument(info, slot, name, inst);
  inst.sampler.volume = unity_level(project_law(project));  // OS-BUG-PASTE24: born at 0 dB under the project's law (50 under the 1.2+ law, 100 under the 1.1.14 table); formats does not know the law
  if (project.set_instrument(slot, inst) != model::Status::Ok) {
    return say(ctx, core::Severity::Warn, "THE MODEL REFUSED THE INSTRUMENT");
  }
  const model::Editor* doc = ctx.session != nullptr ? ctx.session->document() : nullptr;
  if (doc != nullptr && &doc->project() == &project && !doc->in_sync()) {  // the project changed behind the editor's back: as an import does
    const bool had = doc->has_history();
    ctx.session->reset_history();
    if (had) {
      (void)ctx.view.notify.post(core::Severity::Warn, "UNDO HISTORY CLEARED BY THE SAVE", "history", ViewState::kMessageMs);
    }
  }
  ctx.view.instrument = static_cast<int>(slot) + 1;  // the instrument in use is the new one: a pad plays it (when the engine has taken it up)
  core::FixedString<core::kNotifyTextChars> text;
  text.assign("SAVED AS INSTRUMENT ");
  append_dec(text, slot + 1u, 2);
  text.append(": HEARD WHEN STOPPED");
  // OS-AUDIT-71 (E-SREC-07, new Community OS behaviour): the keyboard opens over the new instrument's name (RECORDING, or LOOP nB), as Shift + F1 on the Instrument screen does: F8 keeps the
  // typed name (instrument_rename_accept: one undo unit), F7 leaves the name as saved. The take is saved either way.
  static_assert(sizeof name <= model::kInstrumentNameMax + 1u, "the saved name fits the keyboard's name field");
  TextEntryView& t = ctx.view.text;
  t.entry.begin(name, model::kInstrumentNameMax, TextEntry::Filter::Any);
  t.target = TextEntryView::Target::Instrument;
  t.message = "";
  ctx.view.overlay = OverlayId::TextEntry;
  return say(ctx, core::Severity::Info, text.c_str());
}

}  // namespace o3os::app
