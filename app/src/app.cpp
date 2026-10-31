// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/app.h>
#include <o3os/app/mixer_view.h>

#include "action_handlers.h"
#include "sample_text.h"

#include <o3os/app/module_import.h>
#include <o3os/app/module_ui.h>
#include <o3os/app/og_pcm.h>
#include <o3os/app/og_samples.h>
#include <o3os/app/pad_lights.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/save_service.h>
#include <o3os/app/screens/file_screen.h>
#include <o3os/app/screens/params_screen.h>
#include <o3os/app/screens/sample_load_screen.h>
#include <o3os/app/screens/scene.h>
#include <o3os/app/screens/song_screen.h>
#include <o3os/app/screens/sample_edit_screen.h>
#include <o3os/app/screens/wave_screen.h>
#include <o3os/app/text_format.h>
#include <o3os/app/transient.h>
#include <o3os/app/vel_scale.h>
#include <o3os/model/params.h>

#include <cstring>

namespace o3os::app {

namespace {

// OS-GOV: the strip turns red where the governor stops admitting notes (system_load.h): one number, two places.
static_assert(static_cast<std::uint32_t>(kCpuAlertPercent) == audio::gov::kThresholdPercent, "the red CPU readout starts where the module governor's admission threshold is");

constexpr std::uint32_t kBlinkHalfMs = 250;  // a blink's half period: the Song screen's queued slot
constexpr std::uint16_t kPowerFillStep = 10;  // OS-POWER P1: the line moves in steps of this many thousandths (8 px of the 800)

// What the gesture recogniser asks of the bindings for the context chain in force at the time.
struct HintContext {
  const core::BindingSet* set;
  const core::ContextChain* chain;
};

std::uint8_t hint_wants(const void* ctx, core::InputRef input) noexcept {
  const HintContext& h = *static_cast<const HintContext*>(ctx);
  std::uint8_t wants = core::gesture_wants(*h.set, *h.chain, input);
  if (input == button_ref(Button::Play)) {
    // OS-BATCH-E2E: the built-in Rec + Play chord (E-GLOB-06) does not make Play wait. It is Rec first, then Play inside the window: Rec waits, Play is the partner (it needs no wait of its own to be
    // one). Play first is a plain Play tap, at the press: the transport start is a sound trigger (a pad and a step must stay aligned, the qanet audio probe), not a key to hold back 40 ms.
    // A chord of Play in the user's or a preset's keymap still holds it back (their chords are order-free).
    const core::BindingSet no_builtin{{h.set->layers[0], h.set->layers[1], core::BindingView{nullptr, 0}}};
    const std::uint8_t own = core::gesture_wants(no_builtin, *h.chain, input);
    wants = static_cast<std::uint8_t>((wants & ~core::kWantChord) | (own & core::kWantChord));
  }
  return wants;
}

bool hint_chord(const void* ctx, core::InputRef a, core::InputRef b) noexcept {
  const HintContext& h = *static_cast<const HintContext*>(ctx);
  return core::chord_bound(*h.set, *h.chain, a, b);
}

bool hint_hold_encoder(const void* ctx, core::InputRef button, core::InputRef encoder) noexcept {
  const HintContext& h = *static_cast<const HintContext*>(ctx);
  return core::hold_encoder_bound(*h.set, *h.chain, button, encoder);
}

}  // namespace

App::App(hal::Input& input, const hal::Clock& clock, hal::Display& display, std::size_t dl_capacity_words) noexcept
    : input_(input),
      clock_(clock),
      dl_words_(),
      builder_(display, dl_words_.words,
               dl_capacity_words < eve::kMaxDisplayListWords ? dl_capacity_words : eve::kMaxDisplayListWords) {
  fill_demo(model_.pattern);
  fill_song_demo(model_.song);
  model_.device.init(core::ParamKind::Device);
  theme_generation_ = model_.device.generation(model::device_setting::Theme);  // (the theme starts as the setting's default: Amiga)
  pad_offset_generation_ = model_.device.generation(model::device_setting::PadOffset);  // (the board starts at the setting's default: 12)
  model_.files = &files_;  // the File and Sample load screens read the scan's listing from here (OS-010a, OS-011a)
  model_.peaks = &peak_cache_;  // the sample screens draw the waveform from the App's cache when it is built for the PCM they show (OS-011a)
  gestures_.set_repeat_buttons(kRepeatButtons);  // the arrows (and Delete, OS-DELREPEAT) go on while held, faster the longer (OS-009b)
  const ScreenTable screens = screen_registry();
  for (std::size_t i = 0; i < screens.count; ++i) {  // every screen starts in the mode it names (Pattern: Browse)
    if (screens.items[i].screen != nullptr) {
      state_.mode[screen_index(screens.items[i].id)] = static_cast<std::uint8_t>(screens.items[i].screen->default_mode());
    }
  }
}

void App::run_once() noexcept {
  if (power_down_) {
    // OS-POWER P1: the shutdown is running. No input is read and no screen polls; what finishes a save runs (the save layer, the jobs, their reports) and the strip is drawn.
    const std::uint64_t now = clock_.uptime_ms();
    if (saves_ != nullptr) {
      saves_->poll(session_.jobs, model_.device, now);
    }
    if (session_.jobs.occupied() != 0) {
      (void)session_.jobs.run(clock_, kJobBudgetUs);
      report_jobs();
    }
    if (dirty_) {
      render();
    }
    return;
  }
  // The sequencer's position first (OS-LIVEREC): a live note is placed by where the playhead is when the pad or the key is handled below, not by where it was a pass ago.
  poll_play_position();
  sync_take();  // recording began or ended with it (Play, Stop): the take's undo group follows
  core::InputEvent event{};
  bool drained = false;  // the queue was seen empty: no event older than "now" is still waiting
  if (model_.modules != nullptr) {
    model_.modules->begin_pass();
  }
  for (unsigned n = 0; n < kMaxEventsPerPass; ++n) {
    if (state_.screen == ScreenId::ModuleApp && model_.modules != nullptr && model_.modules->pass_full()) {
      break;  // OS-H3A: the app and its applies have had this pass's share; the events still queued wait for the next pass (and time does not run on: drained stays false)
    }
    if (!input_.poll(event)) {
      drained = true;
      break;
    }
    // OS-STAMPCLAMP: a stamp ahead of the clock (a source that stamps badly) would hold the recogniser's time back, and every hold with it.
    // Clamped here, once, before the INPUT TEST screen, the module app and the gestures see the event; in-range stamps pass bit for bit.
    event.timestamp = core::tick_clamp_to_now(event.timestamp, clock_.now());
    // OS-PATTEMPO (NEW): a pad hit while Pattern is held is a tempo tap, wherever the Pattern + encoder gesture is bound (no modal overlay, no app page). The pad plays no note and starts nothing: it never
    // reaches the pad logic below, nor does its release, and the Pattern tap is consumed as after a turn.
    if (event.kind == core::InputKind::Pad && event.id < kPadCount) {
      const std::uint64_t bit = std::uint64_t{1} << event.id;
      if (event.value == 0 && (tap_pads_ & bit) != 0u) {
        tap_pads_ &= ~bit;
        continue;
      }
      if (event.value > 0) {
        tap_pads_ &= ~bit;  // a press that is not taken as a tap is a normal one: its release is not ours (the bit may be a stale one)
        const core::ContextChain chain = sync_context();  // (a context change forgets the held buttons: Pattern is asked after it)
        const std::uint8_t pattern = static_cast<std::uint8_t>(Button::Pattern);  // (the button's id)
        if (gestures_.down(pattern) && core::hold_encoder_bound(bindings(), chain, button_ref(Button::Pattern), encoder_ref(0))) {
          tap_pads_ |= bit;
          gestures_.consume(pattern);
          state_.tap_tempo.stamp = event.timestamp;
          (void)invoke(core::action_id("song.tempo_tap"), 0);
          continue;
        }
      }
    }
    // OS-SONGMAP: the Song screen's question (1.6) is taken back by any other input, before it is resolved: the input then acts as usual. The answers F1 and F2 are not that.
    if (state_.song.ask.live && song_question_taken_back(state_, event)) {
      song_question_drop(state_);
      dirty_ = true;
    }
    // OS-FWCONFIG: the firmware update dialog's questions are taken back by any other input, before it is resolved (the answers are F1 and, as a step back, F8).
    if (state_.firmware.asking() && firmware_question_taken_back(state_, event)) {
      firmware_question_drop(state_, firmware_);
      dirty_ = true;
    }
    // OS-QAFIX-9: the STEMS page's Delete question lives only while that page stays in view; a page change or leaving Mixer takes it back, so coming back never lets a Delete confirm a question that was not asked now.
    if ((state_.mixer.reset_asked != 0u || state_.mixer.outputs_pending != 0u) && (state_.screen != ScreenId::Mixer || mixer::page_of(state_) != MixerPage::Stems)) {
      state_.mixer.reset_asked = 0;
      state_.mixer.outputs_pending = 0;  // OS-USBOUTS: leaving the STEMS page drops a pending OUTPUTS value
      dirty_ = true;
    }
    // The hidden INPUT TEST screen shows the raw events, before the gestures and bindings that the screen's context swallows.
    if (state_.screen == ScreenId::InputTest && state_.input_test.observe(event, clock_.now())) {
      dirty_ = true;
    }
    // A running app (OS-012b) gets every raw control but the OS's own: the exit chord is a binding of its page, and the Shift layer of its parameters is kept back.
    if (state_.screen == ScreenId::ModuleApp && feed_module_app(event)) {
      dirty_ = true;
    }
    // OS-PADS-A: a pad that the TRACKS layer took is not the screen's: its release must not end a Perform slot, a recorded note or a preview note of another layer.
    bool tracks_pad = false;
    if (event.kind == core::InputKind::Pad && event.id < kPadCount) {
      if (event.value > 0) {
        // The chain below is in force for this very press. A pad that closes the help page is taken too (OS-HELP): its release is the overlay's, and must not end a latched Perform slot, a Rec note or a preview note.
        state_.padmode.pad_down(event.id, pads_tracks_in_force() || (pads_mode_in_force() && pad_mode_takes_pads(state_.padmode.effective())) || state_.overlay == OverlayId::Help);
      } else {
        tracks_pad = state_.padmode.pad_up(event.id);
      }
    }
    if (event.kind == core::InputKind::Pad && event.value == 0 && event.id < kPadCount) {
      state_.take_pads.release(event.id);  // the Record screen's pad playing the take (OS-REC2): the release ends it (pad_up below), and one that never started does not start
      state_.inst_pads &= ~(std::uint64_t{1} << event.id);  // OS-INSTUI S2: the Instrument screen's pad is let go (pad_up below ends its note)
    }
    if (event.kind == core::InputKind::Pad && event.value == 0 && preview_ != nullptr && !tracks_pad) {
      preview_->pad_up(event.id);  // a pad's release ends the note it started (OS-005g); the sink knows which pads it holds
    }
    if (event.kind == core::InputKind::Pad && event.value == 0 && state_.edit.step_hold.row >= 0) {
      (void)invoke(core::action_id("pads.step_up"), event.id);  // OS-PADS-B: a STEP pad's release ends its hold, and clears a lit step that was only tapped
    }
    if (event.kind == core::InputKind::Encoder && state_.dj.filt_pad && state_.overlay == OverlayId::None && state_.dj_page()) {
      (void)invoke(core::action_id("dj.filter"), event.value);  // the DJ page's FILT pad held + the encoder: the filter, ahead of the bindings (a pad has no HoldEncoder; OS-DJ A2)
      continue;
    }
    if (event.kind == core::InputKind::Encoder && state_.dj.scr == DjScr::Tape && state_.overlay == OverlayId::None && state_.dj_page()) {
      (void)invoke(core::action_id("dj.stop_len"), event.value);  // the TAPE STOP pad held + the encoder: the stop's length, ahead of the bindings (OS-DJSTOPLEN)
      continue;
    }
    if (event.kind == core::InputKind::Encoder && state_.edit.step_hold.row >= 0 && state_.overlay == OverlayId::None && state_.screen == ScreenId::Pattern) {
      (void)invoke(core::action_id("pads.step_delta"), event.value);  // a held STEP pad + the encoder: that step's note, ahead of the bindings
      continue;
    }
    if (event.kind == core::InputKind::Button && event.id == static_cast<std::uint8_t>(Button::F5) && event.value == 0 && state_.wav.audition_held) {
      state_.wav.audition_held = false;  // the audition lasts as long as F5 is down (OS-FU2)
      stop_audition();
    }
    if (event.kind == core::InputKind::Button && event.value == 0 && event.id == static_cast<std::uint8_t>(Button::F5) && state_.step_preview) {
      stop_step_preview();  // F5 PREVIEW was let go: the step's note ends (OS-CFGWIRE)
    }
    if (event.kind == core::InputKind::Button && event.value == 0 && event.id == state_.hold.button) {
      state_.hold.button = HeldPreview::kNone;  // the Preview key was let go: the sound ends in this pass's poll_hold (OS-FU3)
      state_.hold.pending = false;
    }
    if (event.kind == core::InputKind::Pad && event.value == 0 && !tracks_pad && state_.screen == ScreenId::Pattern && event.id < model::edit::kPadCount) {
      // A pad's release ends the note it recorded at the playhead (OS-FU2): the note the pad started (OS-PADS-B: a PIANO / ISO pad remembers it, the octave may have moved), else the board's.
      std::uint8_t& held = state_.edit.pad_held[event.id];
      const unsigned note = held != 0u ? held - 1u : state_.edit.pads.note(event.id);
      held = 0;
      if (state_.edit.rec_open != 0u) {
        (void)invoke(core::action_id("rec.note_off"), static_cast<core::ActionArg>(note));
      }
    }
    // OS-QAFIX-E-PERF-08: Master and Song keep the Perform mode on (poll_perform), so a pad held across the change to either still gets its release there: its slot is let go. Only a pad whose press went to
    // the bank (PerformBoard::pads) is forwarded: another pad of the same slot is not, its release would clear the slot's held bit and skip the end of the FX that the first pad started.
    const std::uint64_t perform_bit = event.kind == core::InputKind::Pad && event.id < kPadCount ? std::uint64_t{1} << event.id : 0u;
    const bool perform_held = perform_ != nullptr && (state_.screen == ScreenId::Mixer || state_.screen == ScreenId::Song) && (perform_->pads & perform_bit) != 0u;
    if (event.kind == core::InputKind::Pad && event.value == 0 && !tracks_pad && (state_.screen == ScreenId::Perform || perform_held)) {
      (void)invoke(core::action_id("perform.pad_up"), event.id);  // a Perform pad's release reloads its slot and lets go of it (OS-011b)
    }
    if (perform_ != nullptr && event.value == 0) {
      perform_->pads &= ~perform_bit;  // let go: the next press of this pad starts afresh
    }
    if (event.kind == core::InputKind::Pad && event.value == 0 && state_.dj.pad_held()) {
      (void)invoke(core::action_id("dj.pad_up"), event.id);  // the DJ page's FILT pad let go (whatever is in view now): the filter springs back (OS-DJ A2); a KILL pad: its kill ends (A3); a row 2 pad: its REP / ECHO / CUT ends (B1)
    }
    if (event.kind == core::InputKind::Button && event.value == 0 && state_.dj.nudge_keys != 0u && (event.id == static_cast<std::uint8_t>(Button::Left) || event.id == static_cast<std::uint8_t>(Button::Right))) {
      (void)invoke(core::action_id("dj.nudge_up"), event.id);  // an arrow let go (whatever is in view now): the DJ page's tempo nudge ends (OS-DJ A4)
    }
    if (event.kind == core::InputKind::Button && event.value == 0 && event.id == static_cast<std::uint8_t>(Button::F4) && state_.dj.filt != 0) {
      (void)invoke(core::action_id("dj.key_up"), event.id - static_cast<std::uint8_t>(Button::F1));  // F4 let go: the DJ filter springs back, unless LATCH is on (the gesture layer keeps the release silent after a turn; OS-DJ A2)
    }
    if (event.kind == core::InputKind::Button && event.value == 0 && perform_ != nullptr && event.id >= static_cast<std::uint8_t>(Button::F1) &&
        event.id <= static_cast<std::uint8_t>(Button::F8)) {
      (void)invoke(core::action_id("perform.key_up"), event.id - static_cast<std::uint8_t>(Button::F1));  // a Perform function key's release: the rate back, a tapped scene recalled (OS-PERF2)
    }
    if (event.kind == core::InputKind::Button && event.id == static_cast<std::uint8_t>(Button::Shift)) {
      shift_changed(event.value != 0);  // the Shift layer of the key row, and the end of a block that Shift was extending
    } else if (event.kind == core::InputKind::Encoder || (event.kind == core::InputKind::Button && event.value != 0)) {
      (void)state_.padmode.other_input();  // OS-PADS-A: any other control used under Shift is a chord: the momentary TRACKS layer yields (before the chain is read below)
    }
    const core::BindingSet set = bindings();
    const core::ContextChain chain = sync_context();
    const HintContext hint_ctx{&set, &chain};
    const core::GestureHints hints{&hint_ctx, hint_wants, hint_chord, hint_hold_encoder};
    core::GestureList gestures;
    gestures_.feed(event, hints, gestures);
    dispatch_gestures(gestures);
  }
  if (drained) {
    // Time may only run on once every event stamped before "now" has been seen: after a pass cut short by the
    // cap, a hold must not fire before the release that is still in the queue.
    (void)sync_context();
    core::GestureList due;  // holds, double-tap and chord waits whose time has come
    gestures_.advance(clock_.now(), due);
    dispatch_gestures(due);
  }

  if (state_.screen == ScreenId::InputTest && state_.input_test.set_stats(input_.stats())) {
    dirty_ = true;  // a chip went away or came back, an error was counted
  }

  poll_transport_select();  // (OS-MIDISLV; the position itself is read at the top of the pass: OS-LIVEREC)
  poll_mute_blink();
  poll_modules();
  poll_files();
  if (file_question_sync(state_)) {  // OS-NEWKEEP2
    dirty_ = true;
  }
  poll_export();
  poll_firmware();
  poll_wav();
  poll_audition();
  if (state_.step_preview && state_.screen != ScreenId::Pattern) {
    stop_step_preview();  // another screen came up under the held key
  }
  poll_hold();
  poll_take_pads();
  poll_head();
  if ((pads_mode_in_force() && state_.padmode.fixed() == PadMode::Patterns) || (state_.overlay == OverlayId::PatternChooser && state_.edit.chooser_held)) {
    content_.poll(model_.project);  // the PATS pads' content mask (OS-PADS-B), and the held PATTERN chooser's (OS-PATSWITCH)
  } else if (pads_mode_in_force() && state_.padmode.fixed() == PadMode::Vu && mix_ != nullptr) {
    (void)mix_->poll();  // the VU pads' ballistics (the Mixer page's, once a meter window); the LEDs are read from them, so no redraw
  }
  (void)flash_.poll(model_.mix != nullptr ? model_.mix->tracks : nullptr, clock_.uptime_ms());  // the TRACKS pads' activity flash (OS-PADS-A); no redraw: LEDs only
  poll_theme();
  poll_pad_offset();
  poll_wave();
  poll_sedit();
  poll_samples();
  poll_peaks();
  poll_recorder();
  poll_meters();
  poll_perform();
  poll_dj();
  sync_project_state();  // OS-004d
  if (saves_ != nullptr) {
    saves_->poll(session_.jobs, model_.device, clock_.uptime_ms());  // a save that ended, and the device settings after they have stayed changed (OS-007n)
    if (recorder() != nullptr && saves_->take_written(*recorder())) {
      recorder()->note_saved();  // E-SREC-09: the take's WAV is on the card (the job's Ok end, not its start): F4 DISCARD and a new take need no question until the window moves
    }
    if (state_.xport.running && !state_.xport.armed && state_.xport.slot < 0) {
      state_.xport.slot = saves_->export_slot();  // a set of stems between two files: the next file's job (poll started it, when export_next found no slot free)
    }
  }

  // The jobs get their slice of the pass; what they say (progress, done, failed) goes to the notification queue.
  if (session_.jobs.occupied() != 0) {
    (void)session_.jobs.run(clock_, kJobBudgetUs);
    report_jobs();
  }
  poll_og();
  follow_files();
  sync_recent();

  // The head of the queue runs down while it is on screen (no modal overlay up): the time is the 64-bit uptime, so a
  // notification outlives the 32-bit tick wrap. An expired one is a change on screen.
  if (state_.notify.update(clock_.uptime_ms(), state_.overlay == OverlayId::None)) {
    dirty_ = true;
  }
  if (song_question_sync(state_, model_)) {  // OS-SONGMAP: the Song question that no longer holds (its text ran out, the screen was left, the project changed) is over, after the strip's clock moved
    dirty_ = true;
  }
  // The strip's CPU (the audio's peak over half a second) and MEM (the sample arena's fill): a change of the figures is a redraw (OS-FU3).
  SystemLoad load;
  if (audio_load_ != nullptr) {
    const AudioLoad figures = audio_load_->read();
    load.cpu = cpu_meter_.feed(clock_.uptime_ms(), figures);
    load.level = next_cpu_level(model_.load.level, load.cpu);  // OS-GOV: the chip's colour, with hysteresis
    if (figures.voices_pool != 0u && figures.voices_pool <= 99u) {  // OS-VOX: the voices' peak of the window, and a flash after a steal
      voice_meter_.feed(clock_.uptime_ms(), figures.voices_in_use, figures.steals);
      const std::uint32_t v = voice_meter_.voices();
      load.voices = static_cast<std::int8_t>(v < figures.voices_pool ? v : figures.voices_pool);
      load.pool = static_cast<std::uint8_t>(figures.voices_pool);
      load.steal = voice_meter_.steal(clock_.uptime_ms());
    }
    GovernorWatch::Toast toast;  // OS-GOV: a refused note or a mute is a short toast, at most one every three seconds
    if (gov_watch_.feed(clock_.uptime_ms(), figures, toast)) {
      (void)state_.notify.upsert(toast.severity, toast.text.c_str(), "gov", GovernorWatch::kToastMs);
    }
  }
  if (samples_ != nullptr && samples_->ready()) {
    load.mem = percent_of(samples_->arena_samples() - samples_->free_samples(), samples_->arena_samples());
  }
  if (load != model_.load) {
    model_.load = load;
    dirty_ = true;
  }
  if (dirty_) {
    render();
  }
}

namespace {
// The eleven screen buttons (the ones the global bindings turn into nav.screen): on an app's page they leave it (OS-APPUX S3).
bool is_screen_button(std::uint8_t id) noexcept {
  switch (static_cast<Button>(id)) {
    case Button::Pattern:
    case Button::Song:
    case Button::Params:
    case Button::SamplePlay:
    case Button::SampleEdit:
    case Button::SampleLoad:
    case Button::SampleRec:
    case Button::Master:
    case Button::Performance:
    case Button::File:
    case Button::Config:
      return true;
    default:
      return false;
  }
}
}  // namespace

// An app's raw input (OS-012b). The pads, the buttons (Shift included, which it also sees as a modifier of the events after it) and the encoder go to the module as o3m_ui_event;
// what the OS keeps is the Shift layer of the page's parameters (Shift + arrows, Shift + Delete, Shift + the encoder) while the app declares any, which the page's bindings act on, and F8 (BACK) and Play
// (the transport). An app that declares none draws its own rows (OS-APPUX S2: the grammar of o3m_edit.h), so it gets that layer too. True when the app took the event.
bool App::feed_module_app(const core::InputEvent& event) noexcept {
  if (model_.modules == nullptr || !model_.modules->app_running()) {
    if (model_.modules != nullptr) {
      model_.modules->buttons_down = 0;  // nothing is held for an app that is not there: the key that launched it has its release dropped
      model_.modules->pads_down = 0;
    }
    return false;
  }
  ModuleUi& mu = *model_.modules;
  const bool shift = state_.shift;
  const bool os_column = mu.app_os_column();  // the Shift layer is the OS's parameter column only while the app declares a row and was built below ABI 1.10 (3.2 of OS-APPUX)
  const bool help_up = state_.overlay == OverlayId::Help;  // OS-HELP: the page owns the input while it is up; a key or pad pressed before it opened still has its release delivered below
  switch (event.kind) {
    case core::InputKind::Button: {
      const bool arrow = event.id == static_cast<std::uint8_t>(Button::Up) || event.id == static_cast<std::uint8_t>(Button::Down) || event.id == static_cast<std::uint8_t>(Button::Left) ||
                         event.id == static_cast<std::uint8_t>(Button::Right) || event.id == static_cast<std::uint8_t>(Button::Delete);
      if (shift && arrow && os_column) {
        return false;
      }
      if (is_screen_button(event.id)) {
        return false;  // OS-APPUX S3: a screen button leaves the app (the page's bindings run: Screen::leave ends it); the app never sees it, nor its release
      }
      if (event.value != 0 && (help_up || (shift && event.id == static_cast<std::uint8_t>(Button::Config)))) {
        return false;  // OS-HELP: Shift + Config is the OS's (help.open), and nothing pressed while the help page is up reaches the app
      }
      if (event.id == static_cast<std::uint8_t>(Button::F8) || event.id == static_cast<std::uint8_t>(Button::Play)) {
        return false;  // OS-KEYROWDLG S5: F8 is the page's BACK and Play the transport's, for every app, a 1.1-1.8 one included (it no longer gets either: a change for the apps already on a card): the bindings of the page act, the app never sees them (nor their release, which it never had)
      }
      const bool is_shift = event.id == static_cast<std::uint8_t>(Button::Shift);
      const std::uint64_t bit = event.id < 64u ? std::uint64_t{1} << event.id : 0u;
      if (event.value != 0) {
        mu.buttons_down |= bit;
      } else if ((mu.buttons_down & bit) == 0u) {
        return false;  // the release of a key whose press the app was not given (the launching F1, a Shift layer key)
      } else {
        mu.buttons_down &= ~bit;
      }
      return mu.app_input(O3M_EV_BUTTON, event.id, event.value != 0 ? 1 : 0, is_shift ? event.value != 0 : shift);
    }
    case core::InputKind::Pad: {
      const std::uint64_t bit = event.id < 48u ? std::uint64_t{1} << event.id : 0u;
      if (event.value != 0 && help_up) {
        return false;
      }
      if (event.value != 0) {
        mu.pads_down |= bit;
      } else if ((mu.pads_down & bit) == 0u) {
        return false;
      } else {
        mu.pads_down &= ~bit;
      }
      return mu.app_input(O3M_EV_PAD, event.id, event.value, shift);
    }
    case core::InputKind::Encoder:
      return (!shift || !os_column) && !help_up && mu.app_input(O3M_EV_ENCODER, 0u, event.value, shift);
    default:
      return false;
  }
}

// The page of an app is redrawn 30 times a second while it runs (it may animate: a scope, a meter) and left when the app is gone: a project change clears the modules under it
// (ModuleUi::reset), a stopped app keeps the page (it says so) until the exit chord.
void App::attach_modules(ModuleUi* modules) noexcept {
  model_.modules = modules;
  if (modules != nullptr) {
    modules->set_edit_sink(&App::apply_app_group, this);  // OS-H3A: an app's closed edit group is applied by the action "module.app_group", one undo unit
  }
  wire_app_export();
  wire_app_audio();
  dirty_ = true;
}

void App::wire_app_audio() noexcept {
  if (model_.modules != nullptr) {
    model_.modules->set_play_position(play_position_);
    model_.modules->set_note_sink(preview_);
  }
}

void App::wire_app_export() noexcept {
  if (model_.modules != nullptr) {
    model_.modules->set_export(saves_ != nullptr ? &saves_->app_export() : nullptr);  // OS-H4
  }
}

// OS-H3A: the sink of ModuleUi (set_edit_sink): the app closed an edit group (edit_end, or its call returned with one open), and this runs it as the undoable action "module.app_group" (one undo unit
// named by the app: the unit's argument is the app's catalog entry). Disabled = no document attached to edit.
std::int32_t App::apply_app_group(void* self, std::int32_t entry) noexcept {
  App& a = *static_cast<App*>(self);
  if (a.model_.modules == nullptr) {
    return O3M_EDIT_NO_PROJECT;
  }
  const DispatchResult r = a.invoke(core::action_id("module.app_group"), entry);
  return r == DispatchResult::Disabled ? O3M_EDIT_NO_PROJECT : a.model_.modules->apply_result();
}

void App::poll_modules() noexcept {
  if (model_.modules == nullptr) {
    return;
  }
  if (model_.project != nullptr) {
    model_.modules->sync_slots(*model_.project);  // OS-INSTUI S5: an instance whose slot is no longer a module instrument stays silent (the actions and undo do it at once; this is the safety net)
  }
  std::uint32_t tempo_x100 = 0;
  if (model_.modules->take_tempo_request(tempo_x100)) {  // OS-H1: an app's set_tempo (one value queued, the last call wins): one undoable action, whatever page is in view by now
    (void)invoke(core::action_id("module.app_tempo"), static_cast<core::ActionArg>(tempo_x100));
  }
  if (state_.screen != ScreenId::ModuleApp) {
    return;
  }
  if (!model_.modules->app_running()) {
    if (switch_screen(screen_registry(), model_, state_, app_exit_screen(state_))) {
      dirty_ = true;
    }
    return;
  }
  const bool declared = model_.modules->app_keys_declared();  // OS-KEYROWDLG S5: an app that declares its keys has a Shift layer on the key row (follows_shift)
  if (state_.modules.app_keys != declared) {
    state_.modules.app_keys = declared;
    dirty_ = true;
  }
  const std::uint64_t now = clock_.uptime_ms();
  if (now - model_.modules->frame_ms >= 33u) {
    model_.modules->frame_ms = now;
    dirty_ = true;
  }
}

void App::stop_step_preview() noexcept {
  if (state_.step_preview && preview_ != nullptr) {
    preview_->hold_note(false, 0, 0, 0);  // the Preview key's voice (kLiveSourcePreview, key 1): the release names no note
  }
  state_.step_preview = false;
}

void App::reset_input() noexcept {
  state_.step_preview = false;  // (the notes_off below ends the sound itself)
  state_.wav.audition_held = false;
  stop_audition();
  state_.hold.button = HeldPreview::kNone;  // (OS-FU3) the Preview key's release may have been lost too
  state_.hold.pending = false;
  stop_hold();
  state_.inst_pads = 0;
  tap_pads_ = 0;  // OS-PATTEMPO: a tap's release may have been lost too: the next release of that pad is a normal one
  if (perform_ != nullptr) {
    perform_->pads = 0;  // OS-QAFIX-E-PERF-08: (the notes_off below and the bank's own reset let go of the slots)
  }
  state_.peeking = false;  // (OS-SONGPEEK) the release of the Song key may have been lost: the screen in view stays
  if (preview_ != nullptr) {
    preview_->notes_off(false);  // a release may have been lost with the input: no pad's note may ring on
  }
  gestures_.reset();
  shift_changed(false);  // the gesture layer forgot every button: so does the Shift layer
}

// Shift went down or up (a raw event: the key row and the header marks show the layer while it is held). Its release ends the extension of
// a block that Shift was making (the selection state is cleared).
void App::shift_changed(bool held) noexcept {
  (void)state_.padmode.shift_changed(held);
  if (state_.shift != held) {
    state_.shift = held;
    dirty_ = dirty_ || follows_shift(active_screen(), state_) ||
             state_.overlay == OverlayId::Navigator;  // the screens that follow Shift (key_row, OS-UIFIX), and the Navigator's own key row (OS-NAV2)
  }
  if (!held) {
    Selection& sel = state_.edit.selection;
    sel.extending = false;
    sel.column = false;
  }
}

// The TRACKS layer (pad_layer.h) owns the pads: on Pattern a fixed TRACKS mode or a lone Shift hold, on Perform the lone Shift hold, on Song (OS-SONGPADS) whenever its pads are not in SLOTS mode and the lone Shift hold
// in SLOTS; never under an overlay (a popup, a chooser and the rest have their own pads).
bool App::pads_mode_in_force() const noexcept {
  return state_.screen == ScreenId::Pattern && state_.overlay == OverlayId::None && pad_mode_has_layer(state_.padmode.effective());
}

bool App::pads_tracks_in_force() const noexcept {
  if (state_.overlay != OverlayId::None) {
    return false;
  }
  if (state_.screen == ScreenId::Song) {
    return state_.padmode.song_tracks();
  }
  return (state_.screen == ScreenId::Pattern || state_.screen == ScreenId::Perform) && state_.padmode.tracks(state_.screen == ScreenId::Pattern);
}

// The cursor, the window and the block go back inside the pattern in view, which an action, an undo or a new document may have changed; the Song
// screen's cursor goes back inside the order list the same way (OS-005f).
void App::sync_pattern() noexcept {
  if (pattern_sync(model_, state_)) {
    dirty_ = true;
  }
  if (song_sync(model_, state_) && state_.screen == ScreenId::Song) {
    dirty_ = true;
  }
}

// The sequencer's position, read once a pass (OS-009b). A change that shows on the Pattern screen (the play bar's row, the PLAY state, the tempo the strip
// says) redraws it; in Browse the cursor goes to the step first. Nothing blocks and nothing is allocated: one atomic word and one compare.
void App::poll_play_position() noexcept {
  if (play_position_ == nullptr) {
    return;
  }
  const audio::PlayPosition::Value v = play_position_->read();
  const audio::PlayPosition::Value old = state_.play;
  state_.play = v;
  poll_count_in(v, old.count_in);
  // The badge (PLAY / STOP, and what the Play key toggles from) is the Play key's last accepted request, so that it answers at once; the sequencer then follows a block later. It also follows the
  // sequencer where the sequencer decides on its own: an external MIDI Start, Continue or Stop under a clock slave (OS-MIDISLV), the end of a song that does not loop. Only the sequencer's CHANGES
  // count: comparing levels would flip the badge back for the block or two a Play or Stop takes to arrive.
  if (v.playing != old.playing && state_.playing != v.playing) {
    state_.playing = v.playing;
    dirty_ = true;
    if (!v.playing && pattern_mode_edits(state_.mode[screen_index(ScreenId::Pattern)]) && !state_.edit.rec_free) {
      (void)pattern_set_mode(state_, PatternMode::Browse);  // OS-RECMODE2: the transport stopped by itself (the end of a song): LIVE ends as with Play (STEP stays); the take closes in sync_take
    }
  }
  // OS-PATSWITCH: a pick of the held PATTERN chooser waits for its boundary. It is over when the pattern shows (the landing), when the transport stops, when another pattern starts, and after a few step changes without
  // it (a guard against a command the sequencer never took); until then the view is not pulled back to the pattern that still plays (pattern_follow_play).
  PatternEdit& edit = state_.edit;
  const unsigned queued_was = edit.queued_pattern;
  const bool landing = edit.switch_to != 0u && v.playing && v.pattern == edit.switch_to;
  if (edit.switch_to != 0u) {
    if (landing || !v.playing || v.pattern != old.pattern || (v.step != old.step && ++edit.switch_age >= kSwitchGuardSteps)) {
      edit.switch_to = 0;  // landed; or stopped; or another pattern than the target started (the switch will not land); or the guard ran out
      edit.switch_age = 0;
    }
  }
  if (edit.queued_pattern != 0u) {
    if (!v.playing || v.pattern == edit.queued_pattern) {
      edit.queued_pattern = 0;  // the sequencer started it (or stopped): the PATS pad stops blinking (OS-PADS-B)
    } else if (v.pattern != old.pattern && !landing) {
      edit.queued_pattern = 0;  // OS-PATSWITCH: another pattern started and it was not the queue's: a live jump took the pattern's end and dropped it, so the pad stops blinking. (A pattern that shows as the landing of a switch is not that: a queue made after the switch still waits for the new pattern's end)
    }
  }
  if (!v.playing || v.pattern != old.pattern) {
    state_.edit.rec_open = 0;  // the sequencer stopped, or went on to another pattern: no recorded note is held any more, and no OFF is written for it (OS-FU2, OS-LIVEREC)
  }
  // The live jump that waits (OS-SONG1): the Song screen blinks its slot, so it redraws when the request appears or goes and at each half period of the blink while it waits.
  bool track_wait = false;  // E-PERF-07: a track waits for its loop end to change pattern; the Perform boxes blink like NEXT
  for (unsigned t = 0; t < audio::PlayPosition::kTrackWords; ++t) {
    const std::uint32_t w = v.playing ? play_position_->track_pattern(t) : 0u;
    track_wait = track_wait || (w & 0x10000u) != 0u;
    if (w != state_.track_pat[t]) {
      state_.track_pat[t] = w;
      dirty_ = dirty_ || state_.screen == ScreenId::Perform;
    }
  }
  const bool waiting = (v.playing && v.queued != audio::PlayPosition::kNoOrder) || track_wait;
  const bool phase = waiting && (clock_.uptime_ms() / kBlinkHalfMs) % 2u == 0u;
  if ((phase != state_.blink || (v.queued != old.queued)) && (state_.screen == ScreenId::Song || state_.screen == ScreenId::Perform)) {  // (Perform: F7 NEXT PATTERN blinks, OS-PERF2)
    dirty_ = true;
  }
  state_.blink = phase;
  const bool moved = v.playing != old.playing || (v.playing && (v.pattern != old.pattern || v.step != old.step || v.order != old.order));
  const bool tempo = v.playing && v.tempo_x100 != old.tempo_x100;
  if (state_.dj_page() && (v.playing != old.playing || v.pattern != old.pattern || tempo || edit.queued_pattern != queued_was)) {
    dirty_ = true;  // OS-DJ A4: NOW and NEXT, the pattern row and the tempo on the DJ page (not the step: no redraw a step)
  }
  if (!moved && !tempo) {
    return;
  }
  if (moved && pattern_follow_play(model_, state_)) {
    dirty_ = true;
  }
  const unsigned shown = state_.edit.pattern;
  if (state_.screen == ScreenId::Pattern && (tempo || v.playing != old.playing || play_row(v, shown) != play_row(old, shown))) {
    dirty_ = true;
  }
  // The Song screen marks the song row that plays and the strip says its number.
  if (state_.screen == ScreenId::Song && (v.playing != old.playing || play_order(v) != play_order(old))) {
    dirty_ = true;
  }
  // OS-SONG-VIEW2 V2: the TIMELINE page's playhead and its info line's countdown (bars to the next slot) follow the step, so a step is a redraw there (the other Song pages show no step).
  if (state_.screen == ScreenId::Song && moved && state_.page[screen_index(ScreenId::Song)] == static_cast<std::uint8_t>(SongPage::Timeline)) {
    dirty_ = true;
  }
}

// OS-METRO: the count-in's visuals, from the published position (audio/play_position.h count_in: the steps of it still to run, the running one included). A beat is four steps, so `beat` is the beats
// left, 8..1 for two bars, and the countdown of a bar is 4.. 3.. 2.. 1..: the strip shows the digits so far and one beat later the
// next, rewritten in place (upsert), and each beat flashes the pads, the first of a bar brighter (pad_lights). The notification lives for a beat and is dismissed when the count-in ends, at the start point or on a stop. Counted
// from the position a pass reads, so a pass that misses a step still shows the right digit.
// OS-METROALIGN: the same visuals for the wait of a take armed on a RUNNING transport (no count-in is published then): the steps still to wait come from the position's step and the pattern's length
// (wait_steps_left), shown when at least one beat is left when the take is armed, and over when the take starts or the transport stops. `old_count_in` is the previous pass's: the pass in which a count-in ends
// still has the recorder's last word (a take that waits), whose countdown would be a bar long, so it is skipped. The take itself is not touched: only what is shown.
void App::poll_count_in(const audio::PlayPosition::Value& v, unsigned old_count_in) noexcept {
  ViewState::CountIn& c = state_.count_in;
  unsigned beat = v.playing ? (static_cast<unsigned>(v.count_in) + 3u) / 4u : 0u;
  if (v.playing && v.count_in == 0u && old_count_in == 0u && recorder() != nullptr && model_.project != nullptr) {
    const audio::RecTap::Align wait = recorder()->wait_align();
    if (wait != audio::RecTap::Align::Free) {
      unsigned len = model_.project->pattern_length(v.pattern);
      len = len != 0u ? len : model_.project->default_pattern_length();  // (the sequencer's rule for a missing pattern)
      beat = wait_beat(wait_steps_left(wait, v.step, len), c.beat != 0u);
    }
  }
  if (beat == c.beat) {
    return;
  }
  c.beat = static_cast<std::uint8_t>(beat);
  if (beat == 0u) {
    state_.notify.dismiss("cntin");  // over: the start point, or a stop (the text would otherwise stay for the beat it was posted for)
    dirty_ = true;  // the flash is over with the count-in
    return;
  }
  const unsigned digit = (beat - 1u) % 4u + 1u;  // 4, 3, 2, 1 in each bar
  c.bar = digit == 4u;
  c.at_ms = static_cast<std::uint32_t>(clock_.uptime_ms());
  core::FixedString<core::kNotifyTextChars> text;
  text.assign("RECORDING IN");
  for (unsigned k = 4; k >= digit; --k) {
    const char number[4] = {' ', static_cast<char>('0' + k), '.', '.'};
    text.append(number, sizeof number);
  }
  const std::uint32_t beat_ms = v.tempo_x100 != 0u ? 6000000u / v.tempo_x100 : 500u;
  (void)state_.notify.upsert(core::Severity::Info, text.c_str(), "cntin", beat_ms + 150u);
  dirty_ = true;
}

// What an external Start plays (OS-MIDISLV). The sequencer starts on a master's MIDI Start with no command of ours and plays what it was last told (Command::Select), which was nothing:
// pattern 0, that no project has, so the run was silent and the screen had no playhead. While it is stopped the App says what the Play key would play now, the song on the Song screen and
// the pattern being edited elsewhere, once and again whenever that changes (and after every run: a song moves the sequencer's own choice on). One compare a pass; a sink that cannot take it
// is asked again on the next.
void App::poll_transport_select() noexcept {
  if (transport_ == nullptr || play_position_ == nullptr || state_.play.playing) {
    select_told_ = false;
    return;
  }
  const bool song = state_.screen == ScreenId::Song;
  const std::uint8_t pattern = state_.edit.pattern;
  if (select_told_ && select_song_ == song && select_pattern_ == pattern) {
    return;
  }
  if (transport_->select(song, pattern)) {
    select_told_ = true;
    select_song_ = song;
    select_pattern_ = pattern;
  }
}

// A quantised mute (OS-MUTE) waits for its boundary: its MUTE tag on the Pattern screen blinks (OS-MUTEVIS), so the screen redraws at every half period while one waits, and when the set that waits
// changes (a boundary came, or a toggle was withdrawn). Nothing is redrawn when none waits.
void App::poll_mute_blink() noexcept {
  const TrackSink* sink = model_.mix != nullptr ? model_.mix->tracks : nullptr;
  const std::uint16_t pending = sink != nullptr ? sink->mute_pending() : std::uint16_t{0};
  const bool phase = state_.screen == ScreenId::Pattern && pending != 0u && (clock_.uptime_ms() / kBlinkHalfMs) % 2u == 0u;
  if (state_.screen == ScreenId::Pattern && (pending != mute_pending_seen_ || (pending != 0u && phase != state_.mute_blink))) {
    dirty_ = true;
  }
  state_.mute_blink = phase;
  mute_pending_seen_ = pending;
}

bool App::open_pattern(unsigned id) noexcept {
  if (model_.project == nullptr || id < 1u || id > model::kMaxPatternId || model_.project->pattern(id) == nullptr) {
    return false;
  }
  if (state_.edit.pattern != id) {
    state_.edit.pattern = static_cast<std::uint8_t>(id);
    state_.edit.selection.clear();
  }
  sync_pattern();
  dirty_ = true;
  return true;
}

const Screen& App::active_screen() const noexcept {
  const ScreenEntry* e = find_screen(screen_registry(), state_.screen);
  return e != nullptr && e->screen != nullptr ? *e->screen : kPatternScreen;  // navigation never leaves a missing screen
}

void App::set_theme(ui::ThemeId id) noexcept {
  if (state_.theme != id) {
    state_.theme = id;
    dirty_ = true;
  }
  if (model_.device.get(model::device_setting::Theme) != static_cast<float>(id)) {
    (void)model_.device.set(model::device_setting::Theme, static_cast<float>(id));  // the setting follows (poll_theme sees it already agrees)
  }
  theme_generation_ = model_.device.generation(model::device_setting::Theme);
}

// The device setting THEME moved by any way but set_theme: the Config screen's row, a MIDI controller bound to it, a settings file loaded by the platform. The screen's
// colours follow it; Shift + Master (ui.theme.next) writes the setting itself.
void App::poll_theme() noexcept {
  const std::uint32_t generation = model_.device.generation(model::device_setting::Theme);
  if (generation == theme_generation_) {
    return;
  }
  theme_generation_ = generation;
  const float v = model_.device.get(model::device_setting::Theme);
  const ui::ThemeId id = v >= 2.0f ? ui::ThemeId::White : (v >= 1.0f ? ui::ThemeId::Black : ui::ThemeId::Amiga);
  if (state_.theme != id) {
    state_.theme = id;
    dirty_ = true;
  }
}

// The device setting PAD OFFSET moved: the Config screen's row, a MIDI controller bound to it, a settings file loaded by the platform. The NOTES board keeps its scale and root (apply_entry
// owns them) and takes the new row offset; the pads are read when pressed, so nothing is redrawn.
void App::poll_pad_offset() noexcept {
  const std::uint32_t generation = model_.device.generation(model::device_setting::PadOffset);
  if (generation == pad_offset_generation_) {
    return;
  }
  pad_offset_generation_ = generation;
  PadBoard& pads = state_.edit.pads;
  pads.set(pads.scale(), static_cast<std::uint8_t>(model_.device.get(model::device_setting::PadOffset)), pads.root());
}

// The chain in force. When it differs from the one the gesture layer last saw, a screen, a mode or an overlay has
// changed: what the buttons held down mean is no longer what they meant, so the gesture layer forgets them, with two
// exceptions that are still physically down: Shift (held across a screen change, a mode change or an overlay opening, it
// still modifies the next key: Shift + Pattern opens the Navigator, and the jumps in it want Shift) and a button whose Hold has fired
// (that Hold may be what changed the context, a held F1 opening a chooser, and its release still has to arrive as HoldEnd). A Shift whose release was lost is
// therefore not cleared by a context change; reset_input() clears it.
core::ContextChain App::sync_context() noexcept {
  const core::ContextChain stable = context_chain(false);
  if (stable != last_chain_) {
    gestures_.reset(true);
    last_chain_ = stable;
  }
  return context_chain();
}

// The active contexts, innermost first: the modal overlay (which captures), the screen's mode, the field under its cursor, the screen,
// global. The field level is left out of the chain the gesture layer compares (`with_field` false): moving the cursor from one column to
// another must not make it forget a button that is held (OS-009 D3).
core::ContextChain App::context_chain(bool with_field) const noexcept {
  core::ContextChain chain;
  if (const Overlay* overlay = overlay_of(state_.overlay); overlay != nullptr && overlay->modal()) {
    (void)chain.push(context_id(overlay->context()), true);
  }
  if (pads_tracks_in_force()) {  // OS-PADS-A: innermost (no overlay is up then), so the pads are the layer's and not the mode's or the field's
    (void)chain.push(context_id(Context::PadsTracks));
  } else if (pads_mode_in_force()) {  // OS-PADS-B: the fixed PIANO / ISO / ... mode, innermost too
    (void)chain.push(context_id(Context::PadsMode));
  }
  const Screen& screen = active_screen();
  const ModeTable modes = screen.modes();
  if (modes.count != 0) {
    const unsigned mode = state_.mode[screen_index(state_.screen)];
    (void)chain.push(context_id(modes.items[mode < modes.count ? mode : 0u].context));
  }
  if (with_field) {
    const Context field = screen.field_context(model_, state_);
    if (field != Context::None) {
      (void)chain.push(context_id(field));
    }
  }
  (void)chain.push(context_id(screen.context()), screen.captures());
  (void)chain.push(context_id(Context::Global));
  return chain;
}

void App::dispatch_gestures(const core::GestureList& gestures) noexcept {
  for (const core::Gesture& gesture : gestures) {
    // Resolved one at a time: an action may change what the next gesture means.
    const core::Resolution r = core::resolve(bindings(), context_chain(), gesture);
    if (r.status == core::Resolution::Status::Bound && r.action != core::ActionId::None) {
      (void)invoke(r.action, r.arg, gesture.repeat);
    } else if (r.status == core::Resolution::Status::Captured && press_closes_overlay(gesture)) {
      (void)invoke(core::action_id("ui.back"), 0);  // OS-HELP: the press only closes the overlay (Q3, Q4); Shift is a modifier, not a key, and a release was never a gesture
    }
  }
}

// OS-HELP: an overlay that closes_on_press() is closed by a press its context captured: a button other than Shift, or a pad. Its releases are not gestures here (a pad's, a held key's), so none is swallowed in a way
// that leaves a note or a hold hanging: run_once ends those from the raw event before the bindings are read, as under any other modal overlay.
bool App::press_closes_overlay(const core::Gesture& gesture) const noexcept {
  const Overlay* overlay = overlay_of(state_.overlay);
  if (overlay == nullptr || !overlay->closes_on_press() || gesture.kind != core::GestureKind::Tap || gesture.repeat) {
    return false;
  }
  return gesture.input.kind == core::InputKind::Pad || (gesture.input.kind == core::InputKind::Button && gesture.input.id != static_cast<std::uint8_t>(Button::Shift));
}

DispatchResult App::invoke(core::ActionId id, core::ActionArg arg, bool repeat) noexcept {
  // The base screen owns the input unless a modal overlay is up; the soft-key labels are those of the screen in view.
  ActionContext ctx{model_, state_, active_screen().soft_keys(), clock_.now(), state_.overlay == OverlayId::None, &session_, nullptr, params_out_,
                    preview_, transport_, project_, samples_, perform_, mix_, recorder()};
  ctx.saves = saves_;
  ctx.clip = &clip_;
  ctx.song_clip = &song_clip_;
  ctx.firmware = firmware_;
  ctx.repeat = repeat;
  sync_project_state();  // OS-004d: what the last action changed in the runtime is the project's before this one runs (a Save started now writes it)
  const DispatchResult result = dispatch(action_registry(), id, arg, ctx);
  if (result == DispatchResult::Changed) {
    dirty_ = true;
  }
  if (result == DispatchResult::Disabled && state_.notify.size() != 0) {
    dirty_ = true;  // an undoable action that says NO PROJECT has posted: the strip shows it
  }
  if (state_.screen == ScreenId::SampleEdit && sedit_sync(model_, state_)) {
    dirty_ = true;  // an undo or a crop changed the length under the selection
  }
  sync_pattern();  // whatever ran may have changed the pattern in view under the cursor (a length, another pattern, an undo)
  sync_take();     // Rec on or off, another screen: the take's undo group follows (OS-LIVEREC)
  return result;
}

unsigned App::poll_midi_cc(midi::CcInput& in) noexcept {
  namespace ds = model::device_setting;
  const unsigned in_port = static_cast<unsigned>(model_.device.get(ds::MidiInPort));
  const unsigned in_channel = static_cast<unsigned>(model_.device.get(ds::MidiInChannel));
  unsigned changed = 0;
  midi::CcMsg m;
  for (unsigned n = 0; n < kMidiCcPerPass && in.pop(m); ++n) {
    if (!midi_in_accepts(in_port, in_channel, m.port, m.channel)) {
      continue;
    }
    const core::ActionArg arg = static_cast<core::ActionArg>((static_cast<std::uint32_t>(m.channel) << 16) | (static_cast<std::uint32_t>(m.cc) << 8) | m.value);
    if (invoke(core::action_id("midi.cc"), arg) == DispatchResult::Changed) {
      ++changed;
    }
  }
  return changed;
}

unsigned App::poll_midi_notes(midi::CcInput& in) noexcept {
  namespace ds = model::device_setting;
  poll_play_position();  // OS-LIVEREC: the platform polls the keyboard before the frame's pass: a note is placed by the playhead of now, not of the last pass (which can be a frame old)
  sync_take();
  const unsigned in_port = static_cast<unsigned>(model_.device.get(ds::MidiInPort));
  const unsigned in_channel = static_cast<unsigned>(model_.device.get(ds::MidiInChannel));
  const std::uint32_t drops = in.note_dropped();
  if (drops != midi_note_drops_) {
    midi_note_drops_ = drops;
    if (preview_ != nullptr) {
      preview_->notes_off(true);  // a message was lost: it may have been the off of a held note
    }
  }
  unsigned played = 0;
  midi::NoteMsg m;
  for (unsigned n = 0; n < kMidiNotesPerPass && in.pop_note(m); ++n) {
    if (!midi_in_accepts(in_port, in_channel, m.port, m.channel)) {
      continue;
    }
    // OS-PADS-B: SCALE LOCK snaps the keyboard's notes (the on and the off of a key alike, so a held note ends where it began unless the lock was switched meanwhile)
    const std::uint8_t key = static_cast<std::uint8_t>(m.kind == midi::NoteKind::AllOff ? m.note : state_.edit.entry.locked(m.note));
    if (m.kind == midi::NoteKind::On) {
      state_.edit.last_note = key;
      state_.edit.last_velocity = m.velocity;  // OS-RECMICRO: REC VELOCITY records it (0 is none)
      (void)invoke(core::action_id("step.midi_note"), key);  // Edit on the Pattern screen only (OS-FU1): the note goes into the cursor step (even while playing); in LIVE, at the playhead (OS-FU2, OS-RECMODE2)
      state_.edit.last_velocity = 0;  // only this keyboard note carries it: a scripted or learned step.midi_note records no stale velocity (OS-RECMICRO review)
    } else if (m.kind == midi::NoteKind::Off && state_.edit.rec_open != 0u) {
      (void)invoke(core::action_id("rec.note_off"), key);
    }
    if (preview_ == nullptr) {
      continue;
    }
    const unsigned track = static_cast<unsigned>(state_.pattern.track());
    const unsigned instrument = static_cast<unsigned>(state_.instrument);
    switch (m.kind) {
      case midi::NoteKind::On: {
        // OS-INSTUI S9: VEL> of a sample instrument blends the keyboard's velocity with no velocity (app/vel_scale.h; 100, the default, is the velocity as it is)
        unsigned velocity = m.velocity;
        const model::Instrument* keyed = model_.project != nullptr && instrument >= 1u ? model_.project->instrument(instrument - 1u) : nullptr;
        if (keyed != nullptr && keyed->kind == model::InstrumentKind::Sample) {
          velocity = vel_scaled_velocity(velocity, keyed->vel_sens);
        }
        preview_->midi_note(true, track, instrument, key, velocity, m.channel);
        ++played;
        break;
      }
      case midi::NoteKind::Off:
        preview_->midi_note(false, track, instrument, key, m.velocity, m.channel);
        break;
      case midi::NoteKind::AllOff:
        preview_->notes_off(true);
        break;
    }
  }
  return played;
}

namespace {

// "<name><suffix>" as one strip line: when it is too long it is the file name that is cut, never the suffix (the percentage,
// DONE, the reason a job failed).
void compose(core::FixedString<core::kNotifyTextChars>& out, const char* name, const char* suffix) noexcept {
  std::size_t suffix_len = 0;
  while (suffix[suffix_len] != '\0') {
    ++suffix_len;
  }
  std::size_t name_len = 0;
  while (name[name_len] != '\0') {
    ++name_len;
  }
  const std::size_t room = suffix_len < core::kNotifyTextChars ? core::kNotifyTextChars - suffix_len : 0;
  out.clear();
  out.append(name, name_len < room ? name_len : room);
  out.append(suffix);
}

}  // namespace

bool App::start_import(const ImportRequest& rq) noexcept {
  const auto refuse = [this](const char* text) {
    (void)state_.notify.post(core::Severity::Warn, text, "import", ViewState::kMessageMs);
    dirty_ = true;
    return false;
  };
  if (rq.format == nullptr || rq.format->reader == nullptr || rq.source == nullptr || rq.arena == nullptr) {
    return refuse("NOTHING TO IMPORT WITH");
  }
  if (import_slot_ >= 0 || peak_slot_ >= 0) {
    return refuse("AN IMPORT IS ALREADY RUNNING");
  }
  import_job_.begin(*rq.format->reader, *rq.source, *rq.arena, rq.label, rq.params);
  const int slot = session_.jobs.start(import_job_);
  if (slot < 0) {
    return refuse("TOO MANY JOBS");
  }
  import_slot_ = slot;
  import_arena_ = rq.arena;
  import_kind_ = rq.format->kind;
  import_pcm_ = rq.params.pcm;
  peak_storage_ = rq.peak_storage;
  peak_elems_ = rq.peak_elems;
  shown_percent_[slot] = -1;
  report_jobs();
  return true;
}

// An import ended, however it ended. What it changed in the attached project, it changed behind the editor's back: the undo
// history no longer matches and ends, with a notice (ADR 0009: a change that is not an undo unit cannot be undone). A sample
// that came in whole gets its peak cache built next, as a second job; `chained` says so, and then the strip goes on with
// the peaks instead of saying DONE.
void App::finish_import(core::SlotState state, bool& chained) noexcept {
  chained = false;
  if (wav_slot_ >= 0 && samples_ != nullptr && state == core::SlotState::Done && import_job_.report() != nullptr) {
    // First, so that the history check below sees the document as the import and this fix-up left it.
    const unsigned slot = static_cast<unsigned>(wav_slot_);
    // OS-BUG-PASTE24: the reader (WAV, IFF or Surge wavetable: formats does not know the law) made a fresh instrument at the default level, 50; in a project that plays the 1.1.14 table that is about
    // -20 dB, so it is born at the table's 0 dB, 100, as a recording is. Under the 1.2+ law 50 is 0 dB and nothing changes.
    model::Project& doc_project = samples_->project_arena().project;
    const model::Instrument* born = doc_project.instrument(slot);
    const bool stereo = wav_side_ && import_job_.report()->pcm_side;  // OS-STEREO (b), S8: CH STEREO and the reader found a stereo file: the instrument wants to play stereo
    if (born != nullptr && (born->sampler.volume != unity_level(project_law(doc_project)) || (stereo && born->stereo == 0u))) {
      model::Instrument fresh = *born;
      fresh.sampler.volume = unity_level(project_law(doc_project));
      fresh.stereo = stereo ? std::uint8_t{1} : fresh.stereo;
      (void)doc_project.set_instrument(slot, fresh);
    }
  }
  const model::Editor* doc = session_.document();
  if (doc != nullptr && import_arena_ != nullptr && &doc->project() == &import_arena_->project && !doc->in_sync()) {
    if (wav_slot_ >= 0 && wav_synced_ && state == core::SlotState::Done && session_.state_editor() != nullptr) {
      // OS-BUG-SLDOVER: a Sample load changed one instrument slot and nothing else: the history of the pattern, the song and the other instruments' sample edits stays; only the sound ring ends, and
      // only when one of its records names the slot (Editor::adopt_instrument_load).
      if (session_.state_editor()->adopt_instrument_load(static_cast<std::uint32_t>(wav_slot_))) {
        (void)state_.notify.post(core::Severity::Warn, "UNDO OF INSTRUMENT EDITS CLEARED BY THE LOAD", "history", ViewState::kMessageMs);
      }
    } else {
      const bool had = doc->has_history();
      session_.reset_history();
      if (had) {
        (void)state_.notify.post(core::Severity::Warn, "UNDO HISTORY CLEARED BY THE IMPORT", "history", ViewState::kMessageMs);
      }
    }
  }
  if (wav_slot_ >= 0 && samples_ != nullptr) {  // a Sample load (OS-011a): the audio is the slot's now, and the engine is to take it up
    const formats::FormatReport* rep = import_job_.report();
    const unsigned slot = static_cast<unsigned>(wav_slot_);
    if (wav_batch_.active) {  // OS-AUDIT-A7: a file of the multi-load ended
      if (state == core::SlotState::Done && rep != nullptr) {
        ++wav_batch_.loaded;
      } else {
        ++wav_batch_.failed;
        wav_batch_.next_slot = static_cast<std::uint8_t>(slot);  // a file that did not come in leaves its slot to the next one
      }
    }
    if (state == core::SlotState::Done && rep != nullptr) {
      std::int16_t* const side = rep->pcm_side ? wav_pcm_ + ((rep->pcm_frames + 1u) & ~1u) : nullptr;  // OS-STEREO (b): S follows M in the same block
      (void)samples_->commit_load(slot, wav_pcm_, rep->pcm_frames, side);  // OS-RATEPITCH: the rate is the instrument's now, and the rebind gives the voices its factor: no "PITCH IS OFF" warning
    }
    peaks_slot_ = slot;  // the peak job chained below builds the cache of exactly this PCM
    peaks_version_ = samples_->version(slot);
    samples_->close_wav();
    wav_slot_ = -1;
    wav_pcm_ = nullptr;
    if (state == core::SlotState::Done && wav_side_ && rep != nullptr && !rep->pcm_side) {  // asked for stereo, got the mid alone (a file the reader keeps mono): never silent
      (void)state_.notify.upsert(core::Severity::Info, "NO SIDE SIGNAL: LOADED MONO", "sample", ViewState::kMessageMs);
    }
    wav_side_ = false;
    if (state == core::SlotState::Done && state_.wav.back_to_params && state_.screen == ScreenId::SampleLoad) {  // OS-INSTUI S5: the type chooser sent us here: back to the instrument that took the sample
      state_.instrument = static_cast<int>(slot) + 1;
      (void)switch_screen(screen_registry(), model_, state_, ScreenId::Instrument);
      dirty_ = true;
    }
  }
  if (state == core::SlotState::Done && import_kind_ == formats::FormatKind::Sample && peak_storage_ != nullptr &&
      import_pcm_ != nullptr && import_job_.report() != nullptr && import_job_.report()->pcm_frames != 0) {
    const formats::FormatReport& rep = *import_job_.report();
    const char* label = import_job_.name() + 7;  // past "IMPORT "
    if (peak_job_.begin(peak_cache_, peak_storage_, peak_elems_, import_pcm_, rep.pcm_frames, rep.pcm_channels, label)) {
      const int slot = session_.jobs.start(peak_job_);
      if (slot >= 0) {
        peak_slot_ = slot;
        peaks_quiet_ = false;  // (an import's own peaks say PEAKS ... on the strip, whatever the build before them was)
        shown_percent_[slot] = -1;
        chained = true;
      }
    } else {
      (void)state_.notify.post(core::Severity::Warn, "NO ROOM FOR THE PEAKS", "import", ViewState::kMessageMs);
    }
  }
  import_arena_ = nullptr;
}

// The news of the jobs, through the notification queue (ADR 0009 rev 1: "a progress notification or overlay is shown
// through the 002b notification path"). A running job is one entry that is rewritten, "IMPORT KICK.WAV 45%", and kept up
// by being refreshed on every pass; the screen is redrawn only when the percentage moves. A finished job says how it ended,
// once, and its slot is released. When the line is too long the file name is cut, never the percentage or the reason.
void App::report_jobs() noexcept {
  for (unsigned slot = 0; slot < core::JobRunner::kSlots; ++slot) {
    const core::SlotState st = session_.jobs.state(slot);
    if (st == core::SlotState::Free) {
      continue;
    }
    if (state_.xport.slot >= 0 && static_cast<int>(slot) == state_.xport.slot && st != core::SlotState::Running) {  // OS-WAVEXP: the export's own words
      finish_export(slot, st);
      continue;
    }
    if (saves_ != nullptr && saves_->og_job_slot() == static_cast<int>(slot)) {  // OS-FILES-C C2c: the files of an OG export are quiet (the overlay shows them, poll_og words the end); a failed one is worded there, too
      if (st != core::SlotState::Running) {
        saves_->og_note_job(session_.jobs.stats(slot));  // T16: what the file's steps cost the loop
        shown_percent_[slot] = -1;
        session_.jobs.release(slot);
      }
      continue;
    }
    if (static_cast<int>(slot) == files_slot_) {  // the File screen's scan: quiet, the screen shows it (follow_files redraws it)
      if (st != core::SlotState::Running) {
        files_slot_ = -1;
        session_.jobs.release(slot);
      }
      continue;
    }
    if (saves_ != nullptr && (saves_->silent_slot(slot) || (saves_->quiet_slot(slot) && st != core::SlotState::Failed))) {  // the device settings' own save: not on the strip unless it fails; the RECENT file's: never (OS-FILES part B)
      if (st != core::SlotState::Running) {
        session_.jobs.release(slot);
      }
      continue;
    }
    if (static_cast<int>(slot) == preview_slot_ && st != core::SlotState::Failed) {  // the audition's read: quiet (a failure says why, below)
      if (st != core::SlotState::Running) {
        preview_slot_ = -1;
        finish_audition(st);
        session_.jobs.release(slot);
      }
      continue;
    }
    const core::Job* job = session_.jobs.job(slot);
    core::FixedString<core::kNotifyTextChars> text;
    if (st == core::SlotState::Cancelled && static_cast<int>(slot) == peak_slot_ && peaks_superseded_) {  // a peak build given up because the sample in view changed: not news
      peaks_superseded_ = false;
      peak_slot_ = -1;
      shown_percent_[slot] = -1;
      session_.jobs.release(slot);
      dirty_ = true;
      continue;
    }
    if (static_cast<int>(slot) == peak_slot_ && peaks_quiet_ && st != core::SlotState::Failed) {  // the Params source visual's build: quiet, so that the action's own notice stays on the strip
      if (st != core::SlotState::Running) {
        peak_slot_ = -1;
        shown_percent_[slot] = -1;
        session_.jobs.release(slot);
        dirty_ = true;  // the visual draws the waveform now
      }
      continue;
    }
    if (st == core::SlotState::Running) {
      const unsigned percent = job->progress() / 10u;
      char buf[6];
      unsigned n = 0;
      buf[n++] = ' ';
      if (percent >= 100) {
        buf[n++] = '1';
        buf[n++] = '0';
        buf[n++] = '0';
      } else {
        if (percent >= 10) {
          buf[n++] = static_cast<char>('0' + percent / 10u);
        }
        buf[n++] = static_cast<char>('0' + percent % 10u);
      }
      buf[n++] = '%';
      buf[n] = '\0';
      compose(text, job->name(), buf);
      (void)state_.notify.upsert(core::Severity::Info, text.c_str(), "job", kProgressMs);
      if (shown_percent_[slot] != static_cast<int>(percent)) {
        shown_percent_[slot] = static_cast<int>(percent);
        dirty_ = true;
      }
      continue;
    }
    bool chained = false;
    bool nothing_done = false;  // an effect that found nothing to do says so instead of DONE
    if (static_cast<int>(slot) == pti_slot_) {
      pti_slot_ = -1;
      finish_pti_load(st);
    }
    if (static_cast<int>(slot) == import_slot_) {
      finish_import(st, chained);
      import_slot_ = -1;
    }
    if (static_cast<int>(slot) == peak_slot_) {
      peak_slot_ = -1;
    }
    if (static_cast<int>(slot) == slice_slot_) {
      slice_slot_ = -1;
      finish_slice(st);
    }
    if (static_cast<int>(slot) == edit_slot_) {
      edit_slot_ = -1;
      state_.sedit.running = false;
      state_.sedit.cancel = false;
      dirty_ = true;
      nothing_done = finish_edit(st);
    }
    if (static_cast<int>(slot) == preview_slot_) {  // a failed audition read
      preview_slot_ = -1;
      finish_audition(st);
    }
    if (!chained && !nothing_done) {  // (an import that goes on to its peaks does not say DONE: the peaks do)
      core::Severity severity = core::Severity::Info;
      std::uint32_t ttl = kMessageMs;
      const bool pti_clipped = saves_ != nullptr && saves_->take_pti_clipped(slot);  // (taken at every end, so that the runner's slot is not taken for the .pti save's later)
      unsigned mono_count = 0;
      const char* mono_first = "";
      const bool stereo_mono = saves_ != nullptr && saves_->take_stereo_mono(slot, mono_count, mono_first);  // (taken at every end, like the .pti's)
      if (st == core::SlotState::Done && stereo_mono) {
        // OS-STEREO (b), rule 4: the project is saved, but a stereo instrument went down mono (its side file's name collides with another sample or does not fit): never silent. "STEREO SAVED MONO: NAME (+N)"
        text.assign("STEREO SAVED MONO: ");
        core::FixedString<8> tail;
        if (mono_count > 1u) {
          tail.assign(" +");
          append_uint(tail, mono_count - 1u);
        }
        const std::size_t room = core::kNotifyTextChars - text.size() - tail.size();
        char shown[model::kInstrumentNameMax + 1];
        std::strncpy(shown, mono_first, sizeof shown - 1u);
        shown[sizeof shown - 1u] = '\0';
        if (std::strlen(shown) > room) {
          shown[room] = '\0';
        }
        text.append(shown);
        text.append(tail.c_str());
        severity = core::Severity::Warn;
        ttl = 4000;
      } else if (st == core::SlotState::Done) {
        compose(text, job->name(), pti_clipped ? " DONE (CLIPPED)" : " DONE");  // (OS-FILES-C C1b: the resampler saturated samples)
      } else if (st == core::SlotState::Cancelled) {
        compose(text, job->name(), " CANCELLED");
      } else {
        core::FixedString<core::kNotifyTextChars> suffix;
        suffix.assign(" FAILED");
        const char* why = job->detail();
        if (why[0] != '\0') {
          suffix.append(": ");
          suffix.append(why);
        }
        if (std::strcmp(why, kSsndBeforeCommText) == 0) {  // OS-LOADCH: this reason is the whole strip (after the name it would be cut to nothing); every other reason keeps the name
          text.assign(why);
        } else {
          compose(text, job->name(), suffix.c_str());
        }
        severity = core::Severity::Error;
        ttl = 4000;
      }
      (void)state_.notify.upsert(severity, text.c_str(), "job", ttl);
    }
    shown_percent_[slot] = -1;
    session_.jobs.release(slot);
    dirty_ = true;
  }
}

// ---- The WAV export (OS-WAVEXP) -----------------------------------------------------------------------------------------------------------------

// Once a pass, before the jobs run: an armed export (the transport was told to stop) starts when the sequencer's published position says it has; a running one gives the overlay its bar and its pass; and
// the EXPORT page asks the card for the name its row would make (a directory lookup, only when the row, the pattern or the page changed).
void App::poll_export() noexcept {
  ExportView& x = state_.xport;
  if (x.running) {
    if (x.armed) {
      if (!state_.play.playing) {
        if (x.kind == ExportView::Og) {  // OS-FILES-C C2c: the project as an OG folder; the console's start has the same refusals
          x.armed = false;
          const char* const why = start_og_export();
          if (why != nullptr) {
            x.running = false;
            x.fresh = false;
            state_.overlay = state_.overlay == OverlayId::ExportProgress ? OverlayId::None : state_.overlay;
            // OS-USBDISK 4.3: with the card on the computer the refusal is asked, ahead of the persistent disk strip (as the actions' own refusals are)
            (void)(state_.card_on_computer ? state_.notify.ask(core::Severity::Warn, why, "export", kMessageMs) : state_.notify.upsert(core::Severity::Warn, why, "export", kMessageMs));
          }
        } else {
          (void)invoke(core::action_id("export.run"), 0);
        }
        dirty_ = true;
      }
      return;
    }
    if (x.kind == ExportView::Og) {
      return;  // (poll_og follows it)
    }
    if (saves_ != nullptr) {
      const ExportSource& src = saves_->export_source();
      if (x.stopped[0] == '\0') {  // OS-EXPNOTE: the held live notes the export stopped when it took the engine: kept (the first set of a stems run), said by the strip when the export ends
        std::strncpy(x.stopped, src.stopped(), sizeof x.stopped - 1u);
        x.stopped[sizeof x.stopped - 1u] = '\0';
      }
      const int pm = src.progress_permille();
      const std::uint16_t permille = static_cast<std::uint16_t>(pm < 0 ? 0 : (pm > 1000 ? 1000 : pm));
      const std::uint8_t pass = src.phase() == ExportSource::Phase::Measure || src.phase() == ExportSource::Phase::Idle ? 0u : 1u;
      std::uint8_t stem = x.stem;
      if (x.kind == ExportView::Stems && x.slot >= 0) {  // a set of stems: which file, of how many (the file between two jobs keeps the last one's number)
        stem = static_cast<std::uint8_t>(saves_->stem_index());
        char part[sizeof x.part];
        saves_->stem_now(part, sizeof part);
        if (std::strcmp(part, x.part) != 0) {
          std::strcpy(x.part, part);
          dirty_ = true;
        }
      }
      if (permille / 10u != x.permille / 10u || pass != x.pass || stem != x.stem) {
        dirty_ = true;
      }
      x.permille = permille;
      x.pass = pass;
      x.stem = stem;
      if (x.kind == ExportView::Stems && x.slot < 0 && !saves_->busy()) {  // F8 between two files: the set has ended with no job to say so
        finish_export_idle();
      }
    }
    return;
  }
  if (saves_ == nullptr && x.page && !x.fresh && state_.screen == ScreenId::File && x.row == ExportView::OgProject) {  // no card write layer: nothing to count, and the page says so (not COUNTING)
    x.og_valid = false;
    x.og_why = static_cast<std::uint8_t>(SaveService::Start::NoWriter);
    x.fresh = true;
    dirty_ = true;
  }
  const bool og_row = x.page && saves_ != nullptr && state_.screen == ScreenId::File && x.row == ExportView::OgProject;
  if (!og_row) {
    if (saves_ != nullptr && saves_->og_survey_running()) {
      saves_->og_survey_cancel();  // the row was left before the count ended
      state_.notify.dismiss("file", kOgCountingText);  // (and F1 may have put COUNTING ... WAIT on the strip: it would otherwise stay until kMessageMs runs out)
    }
  } else if (x.fresh && x.og_valid && !saves_->og_survey_current()) {
    x.fresh = false;  // the project or the audio changed since the count: count again
    x.og_valid = false;
    dirty_ = true;
  }
  if (og_row && !x.fresh) {  // OS-FILES-C C2c: the folder, the size and what would be left out, counted a slice a pass (the page says COUNTING until it is done)
    if (!saves_->og_survey_running()) {
      const char* loaded = project_ != nullptr && project_->loaded() ? project_->folder() : "";
      x.og_valid = false;
      x.og = OgSurvey{};
      const SaveService::Start begun = model_.project != nullptr ? saves_->og_survey_begin(*model_.project, samples_, loaded) : SaveService::Start::Empty;
      if (begun != SaveService::Start::Started) {
        x.og_why = static_cast<std::uint8_t>(begun);
        x.fresh = true;
      }
      dirty_ = true;
      return;
    }
    const SaveService::SurveyStep step = saves_->og_survey_step();
    if (step != SaveService::SurveyStep::More) {
      state_.notify.dismiss("file", kOgCountingText);  // (F1 pressed while counting)
      x.og_valid = step == SaveService::SurveyStep::Done;
      if (x.og_valid) {
        x.og = saves_->og_survey_result();
      } else {
        x.og_why = static_cast<std::uint8_t>(saves_->og_survey_why());
      }
      x.fresh = true;
      dirty_ = true;
    }
    return;
  }
  if (x.page && !x.fresh && saves_ != nullptr && state_.screen == ScreenId::File) {
    ExportRequest er;
    if (x.row == ExportView::Song || x.row == ExportView::SongStems) {
      er.what.what = audio::OfflineRequest::What::Song;
    } else {
      er.what.what = audio::OfflineRequest::What::Pattern;
      er.what.pattern = x.pattern;
    }
    er.form = ExportView::stems_row(x.row) ? ExportRequest::Form::Stems : ExportRequest::Form::Mix;
    er.project = model_.project;
    er.loaded_folder = project_ != nullptr && project_->loaded() ? project_->folder() : "";
    char path[hal::storage::save::kPathCap] = {};
    x.preview[0] = '\0';
    if (saves_->export_name(er, path, sizeof path)) {
      std::size_t n = 0;
      for (; path[n] != '\0' && n + 1u < sizeof x.preview; ++n) {
        const char c = path[n];
        x.preview[n] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : c;
      }
      x.preview[n] = '\0';
    }
    x.fresh = true;
    dirty_ = true;
  }
}

// ---- A whole project in the OG format (OS-FILES-C C2a) -----------------------------------------------------------------------------------------------------------------------------

const char* App::start_og_export() noexcept {
  if (saves_ == nullptr) {
    return "NO CARD WRITE LAYER";
  }
  if (model_.project == nullptr) {
    return "NO PROJECT";
  }
  if (model_.rec != nullptr && model_.rec->state() != Recorder::State::Idle) {
    return "A RECORDING OR A TAKE IS IN PROGRESS";
  }
  if (state_.play.playing) {
    return "STOP THE TRANSPORT FIRST";  // (C2c stops it itself)
  }
  if (project_ != nullptr && project_->busy()) {
    return "A PROJECT LOAD IS RUNNING";
  }
  if (session_.jobs.running() != 0u) {
    return "A JOB IS RUNNING";  // a sample edit, a peak build, an import, a listing: the card and the document are theirs for the moment
  }
  const char* loaded = project_ != nullptr && project_->loaded() ? project_->folder() : "";
  const SaveService::Start started = saves_->start_og_export(session_.jobs, *model_.project, loaded, samples_);
  if (started == SaveService::Start::Started) {
    og_watch_ = true;  // poll_og words the end
    return nullptr;
  }
  return SaveService::og_start_text(started, saves_->gate());
}

// A whole-project OG export is followed (the EXPORT page's OG PROJECT row under its overlay, or the console's `ogexp`): while it runs the overlay is told the files done; at its end the strip says how, once, and
// the overlay closes. The set is SaveService's (one job a file, project.mt last); its files are quiet in report_jobs. At the end of a good one the strip gives the folder and the files, and a second line what was
// left out (the same kinds as the question); a stop gives the files finished, of the total.
void App::poll_og() noexcept {
  if (saves_ == nullptr) {
    return;
  }
  ExportView& x = state_.xport;
  const bool ui = x.running && !x.armed && x.kind == ExportView::Og;
  if (!og_watch_ && !ui) {
    return;
  }
  const SaveService::OgState st = saves_->og_state();
  if (st == SaveService::OgState::Running) {
    if (ui) {
      const unsigned done = saves_->og_files_done();
      const unsigned total = saves_->og_files_total();
      const std::uint16_t permille = total != 0u ? static_cast<std::uint16_t>(done * 1000u / total) : std::uint16_t{0};
      if (done != x.og_done || total != x.og_total || permille / 10u != x.permille / 10u) {
        dirty_ = true;
      }
      x.og_done = static_cast<std::uint16_t>(done);
      x.og_total = static_cast<std::uint16_t>(total);
      x.permille = permille;
      x.pass = 1;
    }
    return;
  }
  og_watch_ = false;
  const unsigned done = saves_->og_files_done();
  const unsigned total = saves_->og_files_total();
  const formats::og::ExportDrops& d = saves_->og_drops();
  core::FixedString<core::kNotifyTextChars> text;
  core::Severity severity = core::Severity::Info;
  std::uint32_t ttl = kMessageMs;
  if (st == SaveService::OgState::Done) {
    text.assign("SAVED ");
    char folder[hal::storage::save::kNameMax + 1] = {};
    const char* const slash = std::strrchr(saves_->og_folder(), '/');
    const char* const name = slash != nullptr ? slash + 1 : saves_->og_folder();
    for (std::size_t i = 0; name[i] != '\0' && i + 1u < sizeof folder; ++i) {
      folder[i] = (name[i] >= 'a' && name[i] <= 'z') ? static_cast<char>(name[i] - ('a' - 'A')) : name[i];
    }
    core::FixedString<24> tail;
    tail.assign(" (");
    append_uint(tail, done);
    tail.append(saves_->og_clipped() != 0u ? " FILES, CLIPPED)" : " FILES)");
    const std::size_t room = core::kNotifyTextChars - text.size() - tail.size();
    const std::size_t len = std::strlen(folder);
    if (len <= room) {
      text.append(folder);
    } else {
      text.append("~");
      text.append(folder + (len - (room - 1u)));  // too long for the strip: the end of the name, where the number is
    }
    text.append(tail.c_str());
    ttl = 4000;
  } else if (st == SaveService::OgState::Cancelled) {
    text.assign("OG EXPORT CANCELLED: ");
    append_uint(text, done);
    text.append(" OF ");
    append_uint(text, total);
    text.append(" FILES");
  } else if (st == SaveService::OgState::Idle) {
    text.clear();
  } else {
    text.assign(st == SaveService::OgState::Modified ? "OG EXPORT STOPPED" : "EXPORT FAILED");
    const char* const why = saves_->og_why();
    if (why[0] != '\0') {
      text.append(": ");
      text.append(why);
    }
    core::FixedString<16> count;
    count.assign(" (");
    append_uint(count, done);
    count.append(" OF ");
    append_uint(count, total);
    count.append(")");
    if (text.size() + count.size() <= core::kNotifyTextChars) {
      text.append(count.c_str());
    }
    severity = core::Severity::Error;
    ttl = 4000;
  }
  if (!text.empty()) {
    (void)state_.notify.upsert(severity, text.c_str(), "job", ttl);
    if (st == SaveService::OgState::Done && d.any_lost()) {  // what the files do not hold, as the question said it
      char items[core::kNotifyTextChars];
      constexpr const char* kTail = " NOT KEPT";
      (void)og_loss_line(d, items, static_cast<unsigned>(core::kNotifyTextChars - std::strlen(kTail)));
      core::FixedString<core::kNotifyTextChars> lost;
      lost.assign(items);
      lost.append(kTail);
      (void)state_.notify.post(core::Severity::Warn, lost.c_str(), "export", 4000);
    }
  }
  if (ui) {
    x.running = false;
    x.armed = false;
    x.fresh = false;  // the next folder name on the page
    if (state_.overlay == OverlayId::ExportProgress) {
      state_.overlay = OverlayId::None;
    }
  }
  dirty_ = true;
}

void App::cancel_og_export() noexcept {
  if (saves_ != nullptr) {
    saves_->cancel_og_export(session_.jobs);
  }
}

// The export's job ended. The overlay closes (the live audio is already back: the source let the engine go before the read-back), and the strip says how: SAVED EXPORT/<PROJECT>/<NAME>.WAV, EXPORT
// CANCELLED, or EXPORT FAILED: <why> (the save's own reason: the card full, no card, a render that would not stand down, the recorder armed...). No file is left but a finished one: the atomic save removes its
// temporary file on a cancel and on a failure (app/atomic_save.h).
// A SET OF STEMS (slice B) is several jobs. A file that ended well with another still to make does not close the overlay: the slot is released and the next file's job starts at once (the live audio
// is back for the moment between them, with the project's own mixer: seq/offline.h). The set ends with its last file ("SAVED 9 STEMS EXPORT/..."), or with the first that does not end well, and the
// files already finished stay: "CANCELLED: 3 OF 9 STEMS SAVED", "EXPORT FAILED: <why> (3 OF 9)" (the 3 that are saved).
void App::finish_export(unsigned slot, core::SlotState st) noexcept {
  ExportView& x = state_.xport;
  const bool stems = x.kind == ExportView::Stems && saves_ != nullptr;
  if (saves_ != nullptr) {
    saves_->export_settle();  // the job that ended is taken in: the file is counted, the set goes on or ends
  }
  bool next_refused = false;
  if (stems && st == core::SlotState::Done && saves_->export_more()) {
    shown_percent_[slot] = -1;
    session_.jobs.release(slot);
    x.slot = -1;
    dirty_ = true;
    const SaveService::Start s = saves_->export_next(session_.jobs);
    if (s == SaveService::Start::Started) {
      x.slot = saves_->export_slot();
      return;
    }
    if (s == SaveService::Start::NoJobSlot) {
      return;  // the runner has no slot free this pass: poll() starts it (and the pass after takes its job in)
    }
    next_refused = true;  // the set ended: the next file could not be started (the card was taken for an update, went read-only, or a name was refused)
  }
  const unsigned total = stems ? saves_->stems_total() : 0u;
  const unsigned saved = stems ? saves_->stems_saved() : 0u;
  core::FixedString<core::kNotifyTextChars> text;
  core::Severity severity = core::Severity::Info;
  std::uint32_t ttl = kMessageMs;
  if (st == core::SlotState::Done && !next_refused && (!stems || saved >= total)) {
    text.assign("SAVED ");
    if (stems) {
      append_uint(text, saved);
      text.append(" STEMS ");
    }
    const std::size_t room = core::kNotifyTextChars - text.size();
    const std::size_t len = std::strlen(x.file);
    if (len <= room) {
      text.append(x.file);
    } else {
      text.append("~");
      text.append(x.file + (len - (room - 1u)));  // too long for the strip: the end of the path, where the name is
    }
    ttl = 4000;
  } else if (st == core::SlotState::Cancelled || (st == core::SlotState::Done && !next_refused)) {  // (a Done file of a set that was cancelled as it ended is a cancelled set)
    text.assign("EXPORT CANCELLED");
    if (stems) {
      text.assign("CANCELLED: ");
      append_uint(text, saved);
      text.append(" OF ");
      append_uint(text, total);
      text.append(" STEMS SAVED");
    }
  } else {
    text.assign("EXPORT FAILED");
    const core::Job* job = session_.jobs.job(slot);
    const char* why = job != nullptr && !next_refused ? job->detail() : "";
    if (why[0] != '\0') {
      text.append(": ");
      text.append(why);
    }
    if (stems && saved != 0u) {
      text.append(" (");
      append_uint(text, saved);
      text.append(" OF ");
      append_uint(text, total);
      text.append(")");
    }
    severity = core::Severity::Error;
    ttl = 4000;
  }
  (void)state_.notify.upsert(severity, text.c_str(), "job", ttl);  // (the source of the progress line it replaces)
  if (x.stopped[0] != '\0') {  // OS-EXPNOTE: what the export stopped (a held pad or MIDI note), named right after how it ended: the overlay is modal, so the strip cannot say it earlier
    (void)state_.notify.post(core::Severity::Info, x.stopped, "export-stopped", 4000);
    x.stopped[0] = '\0';
  }
  x.running = false;
  x.slot = -1;
  x.armed = false;
  x.fresh = false;  // the next number on the EXPORT page
  if (state_.overlay == OverlayId::ExportProgress) {
    state_.overlay = OverlayId::None;
  }
  shown_percent_[slot] = -1;
  session_.jobs.release(slot);
  dirty_ = true;
}

// OS-FWCONFIG: once a pass, while the firmware update dialog is open: the check and the install are followed one slice at a time (hal::firmware: a slice reads 2 KiB of the file), and a dialog whose
// overlay was closed by some other path is cancelled so that nothing is left running or waiting. No port: a dialog cannot be open.
void App::poll_firmware() noexcept {
  if (!state_.firmware.open()) {
    return;
  }
  if (firmware_ == nullptr) {
    state_.firmware.close();
    if (state_.overlay == OverlayId::FirmwareUpdate) {
      state_.overlay = OverlayId::None;
    }
    dirty_ = true;
    return;
  }
  if (firmware_poll(state_, *firmware_, clock_.now())) {
    dirty_ = true;
  }
}

// A set of stems that ended with no job to say so: F8 between two files, the set stopped before the next one started. The files finished stay.
void App::finish_export_idle() noexcept {
  ExportView& x = state_.xport;
  core::FixedString<core::kNotifyTextChars> text;
  text.assign("CANCELLED: ");
  append_uint(text, saves_->stems_saved());
  text.append(" OF ");
  append_uint(text, saves_->stems_total());
  text.append(" STEMS SAVED");
  (void)state_.notify.upsert(core::Severity::Info, text.c_str(), "job", kMessageMs);
  x.running = false;
  x.slot = -1;
  x.armed = false;
  x.fresh = false;
  if (state_.overlay == OverlayId::ExportProgress) {
    state_.overlay = OverlayId::None;
  }
  dirty_ = true;
}

// ---- The browser screens: File (OS-010a) and Sample load (OS-011a) ---------------------------------------------------------------------------

void App::stop_files_scan() noexcept {
  rescan_say_ = false;  // (OS-QAFIX-RESCAN: a scan that is stopped tells nothing; the rescan sets it again after this)
  if (files_slot_ >= 0) {
    session_.jobs.cancel(static_cast<unsigned>(files_slot_));   // a Running scan closes its directory; a finished one is simply released
    session_.jobs.release(static_cast<unsigned>(files_slot_));
    files_slot_ = -1;
  }
}

// Starts the scan of whichever listing is wanted: Projects/ with the project filter for the File screen, the folder Sample load is in with the .wav filter.
void App::start_files_scan() noexcept {
  const bool wavs = files_for_ == FilesFor::Wavs;
  const bool modules = files_for_ == FilesFor::Modules;
  if (!wavs && !modules && saves_ != nullptr) {
    saves_->recent_refresh();  // OS-FILES part B: the projects are being listed; the RECENT file of this card is read (a few bytes) before they are
  }
  const DirScan::Begin begun = wavs      ? files_.begin(*storage_, state_.wav.folder, state_.wav.pti ? kPtiFilter : kWavFilter)
                               : modules ? files_.begin(*storage_, state_.import.folder, state_.import.all ? kAllFilesFilter : kModuleFilter)  // (OS-IMPORT-ANY I1)
                                         : files_.begin(*storage_, kProjectsFolder, kProjectFilter);
  if (begun == DirScan::Begin::BadPath) {
    // a folder name the card layer will not take: back to the root, the next pass scans that
    if (wavs) {
      state_.wav.folder[0] = '\0';
    } else if (modules) {
      state_.import.folder[0] = '\0';
    }
  }
  if (begun != DirScan::Begin::Started) {
    return;
  }
  const int slot = session_.jobs.start(files_);
  if (slot < 0) {
    files_.invalidate();  // every job slot is busy (imports): the next pass asks again
    return;
  }
  files_slot_ = slot;
  shown_percent_[slot] = -1;
  if (wavs) {
    state_.wav.list.reset();
    state_.wav.clear_marks();  // OS-AUDIT-A7: a mark is a row of the listing that has just been thrown away
  } else {
    state_.file.reset();
  }
  dirty_ = true;
}

// OS-SDSWAP: the card slot changed. The strip first; then, for the events that change what a listing would say (everything but Reading), the one listing the browser screens
// share is thrown away, as a RESCAN does, so the next pass reads it from the card that is there now (poll_files restarts it when a browser screen is in view).
void App::card_changed(CardEvent event) noexcept {
  if (firmware_ != nullptr && firmware_abandon(state_, firmware_)) {  // OS-FWCONFIG: a check read a card that is gone; a hand-over in progress is not touched
    dirty_ = true;
  }
  switch (event) {
    case CardEvent::Removed: (void)state_.notify.upsert(core::Severity::Warn, "CARD REMOVED", "card", 4000); break;
    case CardEvent::Reading: (void)state_.notify.upsert(core::Severity::Info, "READING CARD", "card", 4000); break;
    case CardEvent::Ready:
      (void)state_.notify.upsert(core::Severity::Info, "CARD READY", "card", kMessageMs);
      if (model_.card != nullptr && model_.card->mounted() && model_.card->volume().kind == hal::storage::FsKind::ExFat) {  // OS-USBDISK 6.3 (F10): the volume reads, the writer refuses exFAT
        (void)state_.notify.upsert(core::Severity::Warn, "EXFAT CARD: READ ONLY", "exfat", 6000);
      }
      break;
    case CardEvent::NotReadable: (void)state_.notify.upsert(core::Severity::Error, "CARD NOT READABLE", "card", 4000); break;
  }
  if (event != CardEvent::Reading) {
    if (saves_ != nullptr) {
      saves_->card_remounted();  // OS-BUG-REMOUNT: what the saves and the bank believe the card holds is checked against the files before it is trusted again (the volume serial may be the same)
    }
    stop_files_scan();
    files_.invalidate();
    state_.file.reset();
    state_.wav.list.reset();
  }
  dirty_ = true;
}

void App::disk_notice(const char* text, std::uint32_t ttl_ms, bool warn) noexcept {
  if (state_.notify.upsert(warn ? core::Severity::Warn : core::Severity::Info, text, "disk", ttl_ms)) {
    dirty_ = true;
  }
}

void App::shot_notice(const char* text, std::uint32_t ttl_ms, bool warn) noexcept {
  if (state_.notify.upsert(warn ? core::Severity::Warn : core::Severity::Info, text, "shot", ttl_ms)) {
    dirty_ = true;
  }
}

void App::set_card_on_computer(bool on) noexcept {
  if (state_.card_on_computer == on) {
    return;
  }
  state_.card_on_computer = on;
  if (model_.modules != nullptr) {
    model_.modules->set_card_on_computer(on);  // OS-USBDISK 4.3: a module is not bound or launched from a card the computer has
  }
  if (on) {
    stop_files_scan();
    files_.invalidate();
    state_.file.reset();
    state_.wav.list.reset();
    state_.xport.page = false;  // OS-USBDISK 4.3: the EXPORT page exists to write to the card; no stale export mode survives the share
  }
  dirty_ = true;
}

void App::set_disk_state(ViewState::DiskView::Phase phase, bool claims_busy, bool audio, bool writable) noexcept {
  ViewState::DiskView& d = state_.disk;
  if (d.phase != phase || d.claims_busy != claims_busy || d.audio != audio || d.writable != writable) {
    d.phase = phase;
    d.claims_busy = claims_busy;
    d.audio = audio;
    d.writable = writable;
    state_.params.armed = false;  // a first press of the row is for the state it was made in
    dirty_ = true;
  }
}

ViewState::DiskView::Request App::take_disk_request() noexcept {
  const ViewState::DiskView::Request r = state_.disk.request;
  state_.disk.request = ViewState::DiskView::Request::None;
  return r;
}

void App::set_disk_keys(DiskKeys keys) noexcept {
  if (state_.disk_keys != keys) {
    state_.disk_keys = keys;
    dirty_ = true;
  }
}

// ---- OS-POWER P1 -----------------------------------------------------------------------------------------------------------------------------

bool App::unsaved_work() noexcept {
  const bool project = saves_ != nullptr && model_.project != nullptr && saves_->dirty(*model_.project);
  const bool take = model_.rec != nullptr && (model_.rec->state() == Recorder::State::Recording || model_.rec->state() == Recorder::State::Review);
  return project || take;
}

void App::set_power(const core::PowerShow& told) noexcept {
  core::PowerShow show = told;
  if (show.fill < 1000u) {
    show.fill = static_cast<std::uint16_t>(show.fill / kPowerFillStep * kPowerFillStep);  // the line moves 8 px at a time: a frame a change would be a frame a millisecond of the hold
  }
  const bool unsaved = show.text == core::PowerText::Hold && unsaved_work();
  if (show != state_.power.show || unsaved != state_.power.unsaved) {
    state_.power.show = show;
    state_.power.unsaved = unsaved;
    dirty_ = true;
  }
}

bool App::export_running() noexcept {
  return state_.xport.running || (saves_ != nullptr && saves_->og_state() == SaveService::OgState::Running);
}

void App::export_stop() noexcept {
  ExportView& x = state_.xport;
  if (x.running && x.armed) {  // the transport has not stopped yet: the export is simply not started
    x.armed = false;
    x.running = false;
    x.fresh = false;
    if (state_.overlay == OverlayId::ExportProgress) {
      state_.overlay = OverlayId::None;
    }
  } else if (saves_ != nullptr) {
    if (x.kind == ExportView::Og || saves_->og_state() == SaveService::OgState::Running) {
      saves_->cancel_og_export(session_.jobs);
    } else {
      saves_->cancel_export(session_.jobs);
    }
  }
  dirty_ = true;
}

void App::power_begin() noexcept {
  if (power_down_) {
    return;
  }
  power_down_ = true;
  if (firmware_ != nullptr) {
    (void)firmware_abandon(state_, firmware_);  // OS-FWCONFIG: a check holds the card the shutdown's save needs; an install in the hand-over is not touched
  }
  if (saves_ != nullptr) {
    saves_->power_begin();
  }
  // The card-reading jobs of the app (a listing, an audition's reader, a sample import, a peak build) are cancelled; the save layer's job is the shutdown's to finish or cancel (stage B).
  const int keep = saves_ != nullptr ? saves_->job_slot() : -1;
  for (unsigned slot = 0; slot < core::JobRunner::kSlots; ++slot) {
    if (static_cast<int>(slot) != keep && session_.jobs.state(slot) == core::SlotState::Running) {
      session_.jobs.cancel(slot);
    }
  }
  stop_audition();
  stop_hold();
  stop_step_preview();
  dirty_ = true;
}

void App::watch_document() noexcept {
  if (saves_ != nullptr && model_.project != nullptr) {
    saves_->watch(*model_.project);
  }
}

void App::sync_take() noexcept { rec_take_sync(session_.state_editor(), model_, state_); }

void App::end_take() noexcept { rec_take_end(session_.state_editor(), state_.edit); }

void App::document_replaced(bool unsaved) noexcept {
  end_take();                 // OS-LIVEREC: a take of the old project ends (its edits were undone with the history, which the platform reset)
  apply_project_state();      // OS-004d: the new project's mixer routing and Perform slots (a project that says nothing sets the defaults)
  normalise_loaded_state();   // OS-FU5: then what the runtime made of them is the project's, so that the baseline below is what the next pass would copy
  if (saves_ != nullptr && model_.project != nullptr) {
    saves_->document_replaced(*model_.project, unsaved);
  }
}

// OS-QAFIX-E-FILE-10 (maintainer, 2026-10-22; new Community OS behaviour): the platform opens the last project at power-on (boot_recent_project) through the very open of the File screen, so the load is the verified one
// (OS-QAFIX-4: a file that cannot load leaves the document as it booted) and poll_files tells the end on the strip: PROJECT <name> OPENED, or PROJECT <name> FAILED: <why>. (The platform holds the app's frames
// while the boot gate and the load run, so the strip's first sight is the end.) False: nothing started (no sink, a load running, the folder refused); the caller opens the Workspace as before.
bool App::boot_open(const char* folder, bool native) noexcept {
  if (project_ == nullptr || folder == nullptr || state_.file.opening) {
    return false;
  }
  if (project_->open(folder, native ? ProjectSink::Format::Native : ProjectSink::Format::Og) != ProjectSink::Start::Started) {
    return false;
  }
  const char* name = folder;
  for (const char* p = folder; *p != '\0'; ++p) {
    name = *p == '/' ? p + 1 : name;
  }
  core::FixedString<kFileNameChars> shown;
  file_display_name(name, shown);
  state_.file.opening = true;
  state_.file.pending = FileView::Pending::Open;
  state_.file.opening_name = shown;
  dirty_ = true;
  return true;
}

// What the browser screens need of the pass, before the jobs run. The one listing belongs to whichever of the two screens was last in view: entering the other
// lets it go (its job is cancelled, the directory closed) and starts that screen's scan over. A rescan the user asked for starts over. The scan starts when
// a browser screen is in view and nothing is listed: the first time, and again after a rescan, or after the screen was left with a scan that had not read the
// folder (no card, no folder, a card error, cancelled): a listing that did is kept for the next visit. The result of a project open is told once the platform's
// load has ended (its frames are held while it runs, so the first pass after it is the one that sees it).
void App::poll_files() noexcept {
  const bool on_file = state_.screen == ScreenId::File;
  const bool on_wav = state_.screen == ScreenId::SampleLoad;
  const FilesFor want = on_file ? (state_.import.page ? FilesFor::Modules : FilesFor::Projects) : (on_wav ? FilesFor::Wavs : files_for_);  // on any other screen what is held stays held
  if (!on_wav && !wav_batch_.active) {
    state_.wav.clear_marks();  // OS-AUDIT-A7: the marks do not outlive the visit to the screen
  }
  model_.import_report = project_ != nullptr ? project_->import_summary() : nullptr;
  model_.project_folder = project_ != nullptr && project_->loaded() ? project_->folder() : "";
  if (on_file) {  // OS-AUDIT-A6 (E-FILE-17): the header's standing "unsaved" mark follows the save layer's dirty(); asked only where it is drawn, so no other screen is redrawn for it
    const bool unsaved = saves_ != nullptr && model_.project != nullptr && saves_->dirty(*model_.project);
    if (unsaved != model_.project_unsaved) {
      model_.project_unsaved = unsaved;
      dirty_ = true;
    }
  }
  if (want != files_for_) {
    if (files_for_ == FilesFor::Wavs && !files_pti_ && state_.wav.reselect[0] == '\0' && files_.state() == DirScan::State::Done) {  // OS-AUDIT-A1 (E-LOAD-23): the browser leaves; its cursor's entry is found again by name when the listing is read again (a pending UP's target stays; a listing still being read has no cursor worth keeping)
      const DirIndex& index = files_.index();
      const int pos = state_.wav.list.pos();
      if (pos >= 0 && static_cast<unsigned>(pos) < index.count() && index.name_length(static_cast<unsigned>(pos)) < sizeof state_.wav.reselect) {
        std::memcpy(state_.wav.reselect, index.name(static_cast<unsigned>(pos)), index.name_length(static_cast<unsigned>(pos)) + 1u);
      }
    }
    stop_files_scan();
    files_.invalidate();
    state_.file.reset();
    state_.wav.list.reset();
    files_for_ = want;
    dirty_ = true;
  }
  if (on_wav && files_pti_ != state_.wav.pti) {  // OS-INSTUI S7: the instrument list and the sample browser are two listings of one scan
    stop_files_scan();
    files_.invalidate();
    state_.wav.list.reset();
    state_.wav.reselect[0] = '\0';
    files_pti_ = state_.wav.pti;
    dirty_ = true;
  }
  const FileImport::Go go = state_.import.go;  // OS-FILES: the IMPORT page's folder requests (the actions only choose; the card and the listing are ours)
  state_.import.go = FileImport::Go::None;
  if (go != FileImport::Go::None && on_file && files_for_ == FilesFor::Modules) {
    const DirIndex& index = files_.index();
    const int pos = state_.file.pos();
    char next[sizeof state_.import.folder];
    bool moved = false;
    if (go == FileImport::Go::Up) {
      path_parent(state_.import.folder);
      moved = true;
    } else if (pos >= 0 && static_cast<unsigned>(pos) < index.count() && index.is_dir(static_cast<unsigned>(pos)) &&
               path_join(next, sizeof next, state_.import.folder, index.name(static_cast<unsigned>(pos)))) {
      std::memcpy(state_.import.folder, next, std::strlen(next) + 1u);
      moved = true;
    }
    if (moved) {
      stop_files_scan();
      files_.invalidate();
      state_.file.reset();
      dirty_ = true;
    }
  }
  bool rescan = false;
  if (state_.file.rescan) {
    state_.file.rescan = false;
    rescan = rescan || on_file;
  }
  if (state_.wav.list.rescan) {
    state_.wav.list.rescan = false;
    rescan = rescan || on_wav;
  }
  if (rescan) {
    stop_files_scan();
    files_.invalidate();
    state_.file.reset();
    state_.wav.list.reset();
    rescan_say_ = true;  // OS-QAFIX-RESCAN: follow_files() tells the count once this scan has ended
    dirty_ = true;
  }
  const DirScan::State st = files_.state();
  if (!on_file && !on_wav) {
    if (st == DirScan::State::NoCard || st == DirScan::State::NoFolder || st == DirScan::State::Failed || st == DirScan::State::Cancelled) {
      files_.invalidate();
      state_.file.reset();
      state_.wav.list.reset();
    }
  } else if (storage_ != nullptr && st == DirScan::State::Idle) {
    start_files_scan();
  }
  if (state_.file.opening && project_ != nullptr && !project_->busy()) {
    state_.file.opening = false;
    const FileView::Pending pending = state_.file.pending;
    state_.file.pending = FileView::Pending::Open;
    // OS-OPENTOPAT (maintainer, 2026-10-17; new Community OS behaviour): a project that opened (or was recovered), and NEW, show the Pattern view; a failed one stays on File with its error. A save never gets here.
    const auto show_pattern = [this]() {
      if (state_.screen == ScreenId::File) {  // (still where the open was asked: the load holds the frames, but a screen the user has left is not taken back)
        (void)switch_screen(screen_registry(), model_, state_, ScreenId::Pattern);
      }
    };
    if (pending == FileView::Pending::New) {  // F3 NEW: no folder to name
      (void)state_.notify.upsert(project_->loaded() ? core::Severity::Info : core::Severity::Error, project_->loaded() ? "NEW PROJECT" : "NEW PROJECT FAILED", "file",
                                 project_->loaded() ? kMessageMs : 4000);
      if (project_->loaded()) {
        show_pattern();
      }
      dirty_ = true;
      return;
    }
    if (pending == FileView::Pending::Import) {  // OS-FILES: a module imported (or refused); the report page has the rest
      const ModuleSummary* sum = project_->import_summary();
      core::FixedString<core::kNotifyTextChars> text;
      if (sum != nullptr && sum->valid) {
        char line[core::kNotifyTextChars + 1];
        module_strip_text(*sum, line, sizeof line);
        text.assign(line);
      } else {
        text.assign(project_->loaded() ? "MODULE IMPORTED" : "MODULE IMPORT FAILED");
      }
      (void)state_.notify.upsert(project_->loaded() ? core::Severity::Info : core::Severity::Error, text.c_str(), "file", project_->loaded() ? kMessageMs : 4000);
      dirty_ = true;
      return;
    }
    const bool recovered = pending == FileView::Pending::Recover;
    const char* folder = project_->folder();
    const bool refused = project_->refused();  // OS-QAFIX-4: the file never reached the document: loaded() and folder() are those of the project that stays open
    const bool opened = project_->loaded() && !refused;
    if (opened && saves_ != nullptr) {
      saves_->note_opened(folder);  // OS-FILES part B: the newest of the RECENT projects
    }
    const char* name = folder;
    for (const char* p = folder; *p != '\0'; ++p) {
      name = *p == '/' ? p + 1 : name;
    }
    core::FixedString<kFileNameChars> shown;
    file_display_name(name, shown);
    core::FixedString<core::kNotifyTextChars> label;
    label.assign("PROJECT ");
    label.append(refused ? state_.file.opening_name.c_str() : shown.c_str());  // (a refused open: folder() is the project that stays open, the name is the one that was asked for)
    core::FixedString<core::kNotifyTextChars> text;
    if (opened) {
      compose(text, label.c_str(), recovered ? " RECOVERED" : " OPENED");  // (a recovery is whole in memory, still named .bak on the card: the next SAVE puts it back)
      (void)state_.notify.upsert(core::Severity::Info, text.c_str(), "file", kMessageMs);
      show_pattern();
    } else {
      core::FixedString<core::kNotifyTextChars> suffix;
      suffix.assign(" FAILED");
      const char* why = project_->detail();
      if (why[0] != '\0') {
        suffix.append(": ");
        for (const char* p = why; *p != '\0'; ++p) {  // the loader's words are not always upper case ("i/o error")
          const char c = (*p >= 'a' && *p <= 'z') ? static_cast<char>(*p - ('a' - 'A')) : *p;
          suffix.append(&c, 1);
        }
      }
      compose(text, label.c_str(), suffix.c_str());
      (void)state_.notify.upsert(core::Severity::Error, text.c_str(), "file", 4000);
    }
    dirty_ = true;
  }
}

// The requests of the Sample load screen's actions (they only choose; the card, the listing and the jobs are ours): into the folder under the cursor, up a
// folder (the scan starts over there), or the file under the cursor into an instrument slot.
void App::poll_wav() noexcept {
  const WavView::Go go = state_.wav.go;
  poll_wav_batch();  // OS-AUDIT-A7: the next marked file, once the one before has come in
  if (go == WavView::Go::None) {
    return;
  }
  state_.wav.go = WavView::Go::None;
  if (state_.screen != ScreenId::SampleLoad || files_for_ != FilesFor::Wavs) {
    return;  // the screen was left between the key and this pass
  }
  if (wav_batch_.active && (go == WavView::Go::Load || go == WavView::Go::LoadMarked)) {
    (void)state_.notify.upsert(core::Severity::Info, "A MULTI-LOAD IS RUNNING", "sample", ViewState::kMessageMs);
    dirty_ = true;
    return;
  }
  if (go == WavView::Go::LoadMarked) {
    begin_wav_batch();
    return;
  }
  if (go == WavView::Go::Load) {
    if (state_.wav.pti) {
      start_pti_load();
    } else {
      (void)start_wav_load(state_.wav.list.pos());
    }
    return;
  }
  if (go == WavView::Go::Preview) {
    start_audition();
    return;
  }
  if (go == WavView::Go::Enter) {
    const DirIndex& index = files_.index();
    const int pos = state_.wav.list.pos();
    char next[sizeof state_.wav.folder];
    if (pos < 0 || static_cast<unsigned>(pos) >= index.count() || !index.is_dir(static_cast<unsigned>(pos)) ||
        !path_join(next, sizeof next, state_.wav.folder, index.name(static_cast<unsigned>(pos)))) {
      return;  // not a folder any more, or a path the browser cannot hold
    }
    std::memcpy(state_.wav.folder, next, std::strlen(next) + 1u);
    state_.wav.reselect[0] = '\0';
  } else {
    // OS-AUDIT-A1 (E-LOAD-06): the folder just left is the last part of the path; the cursor goes onto it when the parent's scan ends (follow_files)
    const char* left = state_.wav.folder;
    for (const char* p = left; *p != '\0'; ++p) {
      left = *p == '/' ? p + 1 : left;
    }
    const std::size_t left_n = std::strlen(left);
    if (left_n < sizeof state_.wav.reselect) {
      std::memcpy(state_.wav.reselect, left, left_n + 1u);
    } else {
      state_.wav.reselect[0] = '\0';
    }
    path_parent(state_.wav.folder);
  }
  stop_files_scan();
  files_.invalidate();
  state_.wav.list.reset();
  if (state_.wav.ask_pos >= 0 && !state_.wav.ask_text.empty()) {  // OS-BUG-SLDOVER: a REPLACE question was about a file of the old folder; its strip text goes with it
    state_.notify.dismiss("sample", state_.wav.ask_text.c_str());
    state_.wav.ask_text.clear();
  }
  state_.wav.ask_pos = -1;
  dirty_ = true;
}

// F1 / F4 of Sample load: the WAV under the cursor through the import job of OS-002c (sample_bank.h opens the file and gives the arena's tail for the audio),
// then, when the strip says DONE, finish_import() makes the audio the slot's and asks for the handover.
bool App::start_wav_load(int pos) noexcept {
  const auto say = [this](core::Severity severity, const char* text) {
    (void)state_.notify.upsert(severity, text, "sample", ViewState::kMessageMs);
    dirty_ = true;
  };
  const DirIndex& index = files_.index();
  if (samples_ == nullptr || !samples_->ready() || storage_ == nullptr) {
    say(core::Severity::Info, "NO SAMPLE MEMORY");
    return false;
  }
  if (pos < 0 || static_cast<unsigned>(pos) >= index.count() || index.is_dir(static_cast<unsigned>(pos))) {
    say(core::Severity::Info, "NO FILE TO LOAD");
    return false;
  }
  if (import_slot_ >= 0 || peak_slot_ >= 0 || samples_->wav_open()) {
    say(core::Severity::Warn, "AN IMPORT IS ALREADY RUNNING");
    return false;
  }
  const char* name = index.name(static_cast<unsigned>(pos));
  char path[hal::storage::kPathMax + 1];
  if (!path_join(path, sizeof path, files_.folder(), name)) {
    say(core::Severity::Warn, "THE PATH IS TOO LONG");
    return false;
  }
  if (state_.wav.channel == kChannelStereo && index.stereo(static_cast<unsigned>(pos)) && model_.project != nullptr) {
    // OS-STEREO (b), S8: the load names the instrument's sample "samples/instrNN.wav"; with no side name for it (another instrument's sample has it) a save could not keep the side signal: refused here, not lost later
    char own[] = "samples/instr00.wav";
    own[13] = static_cast<char>('0' + (state_.wav.slot + 1) / 10);
    own[14] = static_cast<char>('0' + (state_.wav.slot + 1) % 10);
    char side[model::kSamplePathMax + 8];
    if (!stereo_side_path(*model_.project, static_cast<unsigned>(state_.wav.slot), own, side, sizeof side)) {
      say(core::Severity::Warn, "STEREO REFUSED: SIDE FILE NAME TAKEN");
      return false;
    }
  }
  SampleBank::WavPlan plan;
  const hal::storage::Status opened = samples_->open_wav(*storage_, path, plan);
  if (opened != hal::storage::Status::Ok) {
    say(core::Severity::Warn, samples_->free_samples() == 0u ? "THE SAMPLE ARENA IS FULL" : "CANNOT OPEN THE FILE");
    return false;
  }
  core::FixedString<kFileNameChars> shown;
  file_display_name(name, shown);
  core::FixedString<kFileNameChars> label;  // upper case, as the strip's other words ("IMPORT KICK.WAV 45%")
  for (const char* p = shown.c_str(); *p != '\0'; ++p) {
    const char c = (*p >= 'a' && *p <= 'z') ? static_cast<char>(*p - ('a' - 'A')) : *p;
    label.append(&c, 1);
  }
  ImportRequest rq;
  // OS-SAMPFMT: the header decides which reader (plan.format); a file none of them knows goes by its name, and that reader says why it is no good (OS-GRAN: a Surge wavetable has its own)
  rq.format = plan.format != nullptr ? plan.format : (DirScan::ends_with_ci(name, ".wt") ? &samples_->wt_format() : &samples_->wav_format());
  rq.source = plan.source;
  rq.arena = &samples_->project_arena();
  rq.label = label.c_str();
  rq.params.index = state_.wav.slot;
  rq.params.size_hint = plan.size;
  rq.params.name = name;
  rq.params.pcm = plan.pcm;
  rq.params.pcm_capacity = plan.pcm_capacity;
  rq.params.wavetable = state_.wav.wavetable;  // OS-GRAN: F7's way of reading a WAV as a wavetable
  const bool stereo_file = index.stereo(static_cast<unsigned>(pos));
  const bool want_side = stereo_file && state_.wav.channel == kChannelStereo;  // OS-STEREO (b), S8: CH STEREO keeps the mid (= MIX) and the side signal
  rq.params.channel = stereo_file && !want_side ? state_.wav.channel : std::uint8_t{0};  // OS-LOADCH: F8's channel, for a stereo file only
  rq.params.side = want_side;
  rq.peak_storage = samples_->peak_storage();
  rq.peak_elems = samples_->peak_elems();
  if (!start_import(rq)) {  // it has said why
    samples_->close_wav();
    return false;
  }
  wav_slot_ = state_.wav.slot;
  wav_synced_ = session_.document() != nullptr && session_.document()->in_sync();  // finish_import(): a load that finds the history in step keeps it
  wav_pcm_ = plan.pcm;
  wav_side_ = want_side;
  return true;
}

// OS-AUDIT-A7 (SLD-MULTI, E-LOAD-14 / E-LOAD-15): F1 / F4 with files marked. The marked files are loaded one after the other, in list order, each into the next free instrument slot from WavView::slot on
// (an instrument that is there is never replaced, and the search never wraps round to slot 1: it stops at the last slot). One load is the single load above, started from poll_wav() once the one before has
// ended with its peak build (the same reads and SD steps as a hand-repeated F4, as jobs in slices); when the marks are used up or there is no free slot left the strip says how many came in.
void App::begin_wav_batch() noexcept {
  const auto say = [this](const char* text) {
    (void)state_.notify.upsert(core::Severity::Info, text, "sample", ViewState::kMessageMs);
    dirty_ = true;
  };
  if (samples_ == nullptr || !samples_->ready() || storage_ == nullptr) {
    say("NO SAMPLE MEMORY");
    return;
  }
  if (import_slot_ >= 0 || peak_slot_ >= 0 || samples_->wav_open()) {
    say("AN IMPORT IS ALREADY RUNNING");
    return;
  }
  wav_batch_ = WavBatch{};
  wav_batch_.active = true;
  wav_batch_.total = static_cast<std::uint16_t>(state_.wav.mark_count());
  wav_batch_.next_slot = state_.wav.slot;
  poll_wav_batch();
}

void App::poll_wav_batch() noexcept {
  WavBatch& b = wav_batch_;
  if (!b.active) {
    return;
  }
  if (import_slot_ >= 0 || peak_slot_ >= 0 || samples_ == nullptr || samples_->wav_open()) {
    return;  // the file before is still coming in (finish_import() counts it)
  }
  if (state_.screen != ScreenId::SampleLoad || files_for_ != FilesFor::Wavs) {
    end_wav_batch("SCREEN LEFT");
    return;
  }
  const int pos = state_.wav.first_mark();  // (every new scan cleared the marks: a rescan ends the batch here, with what came in)
  if (pos < 0) {
    end_wav_batch(nullptr);
    return;
  }
  int slot = -1;
  if (model_.project != nullptr) {
    for (unsigned i = b.next_slot; i < model::kMaxInstruments; ++i) {
      if (model_.project->instrument(i) == nullptr) {
        slot = static_cast<int>(i);
        break;
      }
    }
  }
  if (slot < 0) {
    end_wav_batch("NO FREE INSTRUMENT");  // the rest stays marked
    return;
  }
  state_.wav.set_mark(static_cast<unsigned>(pos), false);
  state_.wav.slot = static_cast<std::uint8_t>(slot);
  b.next_slot = static_cast<std::uint8_t>(slot + 1);
  if (!start_wav_load(pos)) {  // it has said why (the file did not open): counted, the next one goes on
    ++b.failed;
    b.next_slot = static_cast<std::uint8_t>(slot);
  }
  dirty_ = true;
}

void App::end_wav_batch(const char* why) noexcept {
  const WavBatch b = wav_batch_;
  wav_batch_ = WavBatch{};
  if (!b.active) {
    return;
  }
  peaks_after_batch_ = true;  // (the instrument in view is not the last one loaded: its waveform is built next, and its PEAKS lines would take the strip from the summary)
  core::FixedString<core::kNotifyTextChars> text;
  text.assign("LOADED ");
  append_uint(text, b.loaded);
  text.append(" OF ");
  append_uint(text, b.total);
  if (why != nullptr) {
    text.append(": ");
    text.append(why);
  } else if (b.failed != 0u) {
    text.append(": ");
    append_uint(text, b.failed);
    text.append(" FAILED");
  }
  (void)state_.notify.upsert(b.loaded == b.total ? core::Severity::Info : core::Severity::Warn, text.c_str(), "sample", ViewState::kMessageMs);
  dirty_ = true;
}

// F1 of Sample load in its .pti mode (OS-INSTUI S7): the instrument file under the cursor into the slot the screen was opened for. The same file and arena rules as a WAV (SampleBank::open_wav holds
// the tail and the file); the read is the PtiLoadJob, and finish_pti_load() makes it the slot's.
void App::start_pti_load() noexcept {
  const auto say = [this](core::Severity severity, const char* text) {
    (void)state_.notify.upsert(severity, text, "instrument", ViewState::kMessageMs);
    dirty_ = true;
  };
  const DirIndex& index = files_.index();
  const int pos = state_.wav.list.pos();
  if (samples_ == nullptr || !samples_->ready() || storage_ == nullptr || session_.state_editor() == nullptr) {
    say(core::Severity::Info, "NO SAMPLE MEMORY");
    return;
  }
  if (pos < 0 || static_cast<unsigned>(pos) >= index.count() || index.is_dir(static_cast<unsigned>(pos))) {
    say(core::Severity::Info, "NO FILE TO LOAD");
    return;
  }
  if (import_slot_ >= 0 || peak_slot_ >= 0 || pti_slot_ >= 0 || samples_->wav_open()) {
    say(core::Severity::Warn, "AN IMPORT IS ALREADY RUNNING");
    return;
  }
  const char* name = index.name(static_cast<unsigned>(pos));
  char path[hal::storage::kPathMax + 1];
  if (!path_join(path, sizeof path, files_.folder(), name)) {
    say(core::Severity::Warn, "THE PATH IS TOO LONG");
    return;
  }
  SampleBank::WavPlan plan;
  if (samples_->open_wav(*storage_, path, plan) != hal::storage::Status::Ok) {
    say(core::Severity::Warn, samples_->free_samples() == 0u ? "THE SAMPLE ARENA IS FULL" : "CANNOT OPEN THE FILE");
    return;
  }
  core::FixedString<kFileNameChars> shown;
  file_display_name(name, shown);
  core::FixedString<kFileNameChars> label;
  for (const char* p = shown.c_str(); *p != '\0'; ++p) {
    const char c = (*p >= 'a' && *p <= 'z') ? static_cast<char>(*p - ('a' - 'A')) : *p;
    label.append(&c, 1);
  }
  PtiLoadJob& job = samples_->pti_job();
  job.begin(*plan.source, samples_->project_arena(), plan.size, plan.pcm, static_cast<std::uint32_t>(plan.pcm_capacity), state_.wav.slot, label.c_str());
  const int slot = session_.jobs.start(job);
  if (slot < 0) {
    samples_->close_wav();
    say(core::Severity::Warn, "TOO MANY JOBS");
    return;
  }
  pti_slot_ = slot;
  pti_target_ = state_.wav.slot;
  pti_pcm_ = plan.pcm;
  shown_percent_[slot] = -1;
  report_jobs();
}

// The instrument-file load ended, however it ended. Done: the audio is the slot's (with its swap on record) and the instrument is put in by the Editor as the unit "instrument.load", which undo and
// redo follow with the audio (undo_or_redo); the Instrument screen comes back on that slot. Otherwise nothing of the project or the bank changed. The file is closed either way.
void App::finish_pti_load(core::SlotState state) noexcept {
  const unsigned slot = pti_target_;
  PtiLoadJob& job = samples_->pti_job();
  model::Editor* ed = session_.state_editor();
  bool loaded = false;
  // A file that decodes to exactly the instrument the slot holds changes nothing in the model, so no unit would be recorded: the load is refused before the audio is committed (a swap with no unit
  // would be taken by the undo of another one, and the arena would be spent on a duplicate), as PASTE says ALREADY HOLDS THIS.
  const model::Instrument* held = model_.project != nullptr ? model_.project->instrument(slot) : nullptr;
  const bool same = state == core::SlotState::Done && held != nullptr && std::memcmp(held, &job.instrument(), sizeof *held) == 0;
  if (same) {
    core::FixedString<core::kNotifyTextChars> text;
    text.assign("INS ");
    text.append(&"0123456789"[((slot + 1u) / 10u) % 10u], 1);
    text.append(&"0123456789"[(slot + 1u) % 10u], 1);
    text.append(" ALREADY HOLDS THIS");
    (void)state_.notify.upsert(core::Severity::Info, text.c_str(), "instrument", kMessageMs);
  }
  if (state == core::SlotState::Done && !same && ed != nullptr && samples_->commit_load_undoable(slot, pti_pcm_, job.frames())) {
    ed->begin_group(core::action_id("instrument.load"), 0);
    const model::Status st = ed->set_instrument(slot, job.instrument());
    ed->end_group();
    loaded = st == model::Status::Ok;
    if (!loaded) {
      (void)samples_->audio_undone(slot);  // the model refused it (the reader checked it as the model does, so this is a bug, not input): the slot's audio goes back
    }
  }
  samples_->close_wav();
  pti_pcm_ = nullptr;
  if (loaded || same) {
    state_.instrument = static_cast<int>(slot) + 1;
    if (state_.screen == ScreenId::SampleLoad && state_.wav.back_to_params) {
      (void)switch_screen(screen_registry(), model_, state_, ScreenId::Instrument);
      (void)kParamsScreen.repress(model_, state_);
    }
  }
  dirty_ = true;
}

// The bank's handover, once a pass (sample_bank.h, THE HANDOVER): playback stopped (the App's own flag and the engine's published position agree), no live note queued or
// held (PreviewSink::live_idle), voices quiet.
void App::poll_samples() noexcept {
  if (samples_ == nullptr) {
    return;
  }
  const bool stopped = !state_.playing && !state_.play.playing;
  const bool live_idle = preview_ == nullptr || preview_->live_idle();  // asked first: nothing is pushed while this pass is in poll(), so an empty queue stays empty
  // OS-LIVEP review: the render's ParamChange drain also writes the engine's descriptors (apply_param). The handover waits until the ring is empty, so the two writers never
  // overlap: the main loop is the ring's one producer and does not push inside poll(), and on the MK66 the render (an interrupt) has finished its drain whenever this code
  // runs, so an empty ring here means no change is half-applied; an entry the ring refused is healed by the rebuild that follows.
  const bool params_idle = params_out_ == nullptr || params_out_->empty();
  if (samples_->poll(clock_.uptime_ms(), stopped, live_idle && params_idle, session_.jobs.running() == 0u, params_idle)) {
    dirty_ = true;
  }
  if (samples_->rebind_pending() != rebind_seen_) {  // the card's "sounds when stopped" appears and goes
    rebind_seen_ = samples_->rebind_pending();
    dirty_ = true;
  }
}

// The Perform bank, once a pass (OS-011b). The screen was just entered: nothing is held and the live values are the stored ones again. A screen other than Perform, Master or Song
// was just entered: every FX ends (those two keep the mode; a trip through them to anywhere else ends it). Then the commands the ring refused earlier go out.
void App::poll_perform() noexcept {
  if (perform_ == nullptr) {
    return;
  }
  seq::PerformBank& bank = perform_->bank;
  if (state_.screen != perform_screen_) {
    perform_screen_ = state_.screen;
    perform_->keys = PerformKeys{};  // OS-PERF2: a function key held across a screen change is forgotten: its release finds nothing, and the step rate is the song's again
    bank.set_rate(0);
    if (state_.screen == ScreenId::Perform) {
      bank.enter();
      dirty_ = true;
    } else if (state_.screen != ScreenId::Mixer && state_.screen != ScreenId::Song) {
      (void)bank.turn_off(perform_->sink());  // idempotent: a mode that is already off stays off (the rows stay selected)
    }
  }
  // A scene key held for the hold time stores the scene (OS-PERF2); its release then recalls nothing.
  PerformKeys& keys = perform_->keys;
  for (unsigned i = 0; i < seq::kPerfScenes && keys.down != 0u && state_.screen == ScreenId::Perform; ++i) {
    const std::uint8_t bit = static_cast<std::uint8_t>(1u << i);
    if ((keys.down & bit) != 0u && (keys.fired & bit) == 0u && core::tick_elapsed(clock_.now(), keys.at[i]) >= core::ticks_from_ms(PerformKeys::kSceneHoldMs)) {
      keys.fired = static_cast<std::uint8_t>(keys.fired | bit);
      (void)bank.store_scene(i);
      static constexpr const char* kStored[seq::kPerfScenes] = {"SCENE 1 STORED", "SCENE 2 STORED", "SCENE 3 STORED"};
      (void)state_.notify.post(core::Severity::Info, kStored[i], "perform", kMessageMs);
      dirty_ = true;
    }
  }
  (void)bank.apply(perform_->sink());
}

// OS-DJ A1, A2, once a pass: the DJ page's crossfader and filter into the engine. While the page is in view the groups' cut follows the fader; the moment it is not (another page, another screen) every DJ
// effect ends: the fader is centred, the curve back to SMOOTH, the filter at 0 and LATCH off (only the groups stay), and the engine is told unity for every track (glided in 20 ms) and the filter 0 (120 ms).
// A push the ring refused is retried the next pass.
void App::poll_dj() noexcept {
  const bool active = state_.dj_page();
  if (!active && !state_.dj.at_rest()) {
    state_.dj.leave();
  }
  if (perform_ != nullptr || transport_ != nullptr) {  // OS-DJ B2: a TRANS's timer against the play position, before the filter and the echo are told below
    dj_trans_publish(state_.dj, state_.play, dj_loop_steps(model_.project, state_.play.pattern, perform_ != nullptr ? &perform_->bank : nullptr), state_.edit.queued_pattern, transport_,
                     model_.project != nullptr ? model_.project->order_ops() : 0u);
  }
  (void)dj_publish(state_.dj, active, params_out_);
  dj_scratch_poll(state_.dj, active, samples_, params_out_);  // OS-DJ C1: the stop told to the engine, the arena's tail given back when it has let go
  if (perform_ != nullptr) {  // OS-DJ A4: the tempo nudge, as the sequencer's performance tempo (the same ring as the Perform slots' commands)
    const bool slave = model_.device.get(model::device_setting::ClockMode) == 2.0f;
    (void)dj_tempo_publish(state_.dj, active, state_.play.playing, slave, state_.play.tempo_x100, perform_->bank.rate(), perform_->sink());
    if (dj_fx_publish(state_.dj, active, static_cast<std::uint32_t>(clock_.uptime_ms()), state_.play.tempo_x100, perform_->bank)) {  // OS-DJ B1: row 2 into the Perform layer; the commands go out now, a refused one with the next pass's apply
      (void)perform_->bank.apply(perform_->sink());
    }
  }
}

// The engine's meters into the mixer board's ballistics (OS-011b), once a pass and only while a page that draws them is in view (a lock-free snapshot every 32 ms: the
// render publishes a window, the bars follow it). A bar that moved is a frame.
void App::poll_meters() noexcept {
  if (mix_ == nullptr || state_.overlay != OverlayId::None) {
    return;
  }
  if (state_.screen == ScreenId::Record) {  // the Record screen's input meter: the REC bus (or MAIN, for a master resample) from the same snapshot
    if (mix_->poll()) {
      dirty_ = true;
    }
    return;
  }
  if (state_.screen == ScreenId::Pattern) {
    poll_pattern_meters();
    return;
  }
  if (state_.screen != ScreenId::Mixer) {
    return;
  }
  const MixerPage page = mixer::page_of(state_);
  if ((page == MixerPage::Mixer || page == MixerPage::Routing) && mix_->poll()) {
    dirty_ = true;
  }
}

// The Pattern screen's track headers (OS-PVU): the meters of the visible tracks are the engine's, from the same snapshot and ballistics as the Mixer's. The board is polled here
// too (a window is read once, so a pass that the VU pads already polled in is no harm) and a frame is asked for only when a visible track's bar or peak tick differs from the one
// last asked for: the ballistics run once per 32 ms window, so a moving signal is at most about 31 frames a second and a silent or steady one none. The demo (no document)
// has fixed levels and never moves.
void App::poll_pattern_meters() noexcept {
  if (model_.project == nullptr) {
    return;
  }
  (void)mix_->poll();
  const int left = state_.pattern.left_track();
  const int shown = static_cast<int>(model_.project->active_tracks()) - left;
  for (int s = 0; s < state_.pattern.visible_tracks(); ++s) {
    const MixBoard::Bar bar = s < shown ? mix_->track_bar(static_cast<unsigned>(left + s)) : MixBoard::Bar{};
    if (bar.level != pattern_bars_[s][0] || bar.peak != pattern_bars_[s][1]) {
      pattern_bars_[s][0] = bar.level;
      pattern_bars_[s][1] = bar.peak;
      dirty_ = true;
    }
  }
}

// The recorder (OS-011c), once a frame: it owns the engine's input path while the Record screen is in view (or a take is armed), points the codec at the source, follows
// the take and the radio. A change is a frame only when the screen that shows it is in view.
void App::poll_recorder() noexcept {
  if (recorder() == nullptr) {
    return;
  }
  const bool in_view = state_.screen == ScreenId::Record;
  if (recorder()->poll(clock_.uptime_ms(), in_view, rec_mode_of_page(state_.page[screen_index(ScreenId::Record)]), state_.play.playing, preview_ == nullptr || preview_->live_idle()) && in_view) {
    dirty_ = true;
  }
  if (recorder()->save_timed_out()) {  // the render did not acknowledge within kSaveWaitMs: the wait ends, nothing was saved, the loop stays in review
    recorder()->cancel_save();
    (void)state_.notify.post(core::Severity::Warn, "SAVE TIMED OUT: TRY AGAIN", "record", ViewState::kMessageMs);
    dirty_ = true;
  }
  if (recorder()->save_ready()) {  // OS-LOOPER L2c: the render has acknowledged the end of the overdub session: the second step of SAVE (record.save commits)
    if (in_view) {
      (void)invoke(core::action_id("record.save"), 0);
    }
    if (recorder()->save_waiting()) {
      recorder()->cancel_save();  // the save refused after the wait, or the screen was left in its few milliseconds: the loop is still in review
    }
  }
}

// The Wave / Chop screen's requests: AUTO starts the transient slicer over the PCM of the instrument in view, as a job ("SLICE BREAK_A 40%" in the strip).
void App::poll_wave() noexcept {
  if (!state_.wave.auto_go) {
    return;
  }
  state_.wave.auto_go = false;
  const auto say = [this](const char* text) {
    (void)state_.notify.post(core::Severity::Info, text, "wave", ViewState::kMessageMs);
    dirty_ = true;
  };
  const unsigned slot = wave_slot(state_);
  const SampleBank::Pcm pcm = samples_ != nullptr ? samples_->pcm(slot) : SampleBank::Pcm{};
  if (state_.screen != ScreenId::Wave || pcm.frames == 0u) {
    return;
  }
  if (slice_slot_ >= 0) {
    say("THE SLICER IS ALREADY RUNNING");
    return;
  }
  const model::Instrument* in = model_.project != nullptr ? model_.project->instrument(slot) : nullptr;
  core::FixedString<24> label;
  for (const char* p = in != nullptr && in->name[0] != '\0' ? in->name : "SAMPLE"; *p != '\0' && label.size() < 16; ++p) {
    const char c = (*p >= 'a' && *p <= 'z') ? static_cast<char>(*p - ('a' - 'A')) : *p;
    label.append(&c, 1);
  }
  if (!samples_->slice_job().begin(pcm.data, pcm.frames, samples_->onset_storage(), label.c_str())) {
    return;
  }
  const int job = session_.jobs.start(samples_->slice_job());
  if (job < 0) {
    say("TOO MANY JOBS");
    return;
  }
  slice_slot_ = job;
  slice_target_ = slot;
  slice_version_ = samples_->version(slot);
  shown_percent_[job] = -1;
}

// The slicer ended. A good answer for the sample it started on is chosen into slices and handed to wave.auto_apply (one undo unit); one for a sample that was
// replaced or edited meanwhile is thrown away.
void App::finish_slice(core::SlotState state) noexcept {
  if (state != core::SlotState::Done || samples_ == nullptr || samples_->version(slice_target_) != slice_version_) {
    return;
  }
  WaveState& ws = state_.wave;
  ws.auto_count = static_cast<std::uint8_t>(choose_slices(samples_->slice_job().detector(), kMaxSliceCount, samples_->auto_slices(), samples_->slicer_scratch()));
  ws.auto_slot = static_cast<std::uint8_t>(slice_target_);
  ws.auto_ready = true;
  (void)invoke(core::action_id("wave.auto_apply"), 0);
  ws.auto_ready = false;
}

// The Sample edit screen's request: the effect starts as a job over the PCM of the instrument in view, writing into a free buffer of the bank's edit pool
// (never over the audio the voices may read: screens/sample_edit_screen.h, THE EDIT PROTOCOL).
void App::poll_sedit() noexcept {
  SampleEditState& st = state_.sedit;
  if (st.cancel) {  // OS-SEDIT-CANCEL: F8 while the effect runs; report_jobs says "<EFFECT> CANCELLED" and frees the slot, finish_edit keeps the audio as it was
    st.cancel = false;
    if (edit_slot_ >= 0) {
      session_.jobs.cancel(static_cast<unsigned>(edit_slot_));
    }
  }
  if (!st.go) {
    return;
  }
  st.go = false;
  const auto say = [this](core::Severity severity, const char* text) {
    (void)state_.notify.post(severity, text, "edit", ViewState::kMessageMs);
    dirty_ = true;
  };
  const unsigned slot = wave_slot(state_);
  const SampleBank::Pcm pcm = samples_ != nullptr ? samples_->pcm(slot) : SampleBank::Pcm{};
  if (state_.screen != ScreenId::SampleEdit || pcm.frames == 0u) {
    return;
  }
  if (edit_slot_ >= 0 || import_slot_ >= 0) {
    say(core::Severity::Warn, "ANOTHER JOB IS RUNNING");
    return;
  }
  bool src_amiga = false;
  std::uint32_t src_rate = 0;
  if (st.op == EditOp::Amiga) {
    const model::Instrument* in = samples_->project().instrument(slot);
    src_amiga = in != nullptr && samples_->engine_for(slot, *in) == audio::kEngineAmiga;
    src_rate = in != nullptr ? in->sample_rate : 0u;  // OS-RATEPITCH: a Modern sample plays at its own rate
  }
  EditSpec spec;
  if (st.op == EditOp::Amiga) {
    spec = make_amiga_spec(pcm.frames, st.period, src_amiga, st.q8, src_rate);
  } else if (st.op == EditOp::Filter || st.op == EditOp::Compressor) {  // OS-AUDIT-15: the FX page's values; the cutoff and the time constants are in Hz and ms of the sample's own rate
    const model::Instrument* in = samples_->project().instrument(slot);
    const unsigned rate = in != nullptr ? in->sample_rate : 0u;
    const std::int16_t* fxv = st.fx;
    spec = st.op == EditOp::Filter ? make_filter_spec(pcm.frames, st.a, st.b, rate, static_cast<FilterType>(fxv[kFxType]), static_cast<unsigned>(fxv[kFxCutoff]), static_cast<unsigned>(fxv[kFxQ]))
                                   : make_comp_spec(pcm.frames, st.a, st.b, rate, fxv[kFxThreshold], static_cast<unsigned>(fxv[kFxRatio]), static_cast<unsigned>(fxv[kFxAttack]),
                                                    static_cast<unsigned>(fxv[kFxRelease]), fxv[kFxMakeup], fxv[kFxMakeup] < 0, fxv[kFxDetector] != 0);
    if (in != nullptr) {  // OS-A9-SEAM: the instrument in view loops (forward or backward; ping-pong has no jump) and the range starts at its loop start: the state is warmed over the loop's tail
      const OgLoop loop = og_loop_of(*in, pcm.frames);
      if (loop.active && loop.mode != 3u) {
        set_loop_warmup(spec, pcm.frames, loop.start, loop.end, rate);
      }
    }
  } else {
    spec = make_spec(st.op, pcm.frames, st.a, st.b, st.amp);
  }
  std::int16_t* dst = samples_->edit_begin(slot, edit_result_frames(spec, pcm.frames));
  if (dst == nullptr) {
    say(core::Severity::Warn, "NO ROOM FOR THE EDIT");
    return;
  }
  static constexpr const char* kNames[] = {"CROP", "NORMALISE", "REVERSE", "FADE IN", "FADE OUT", "AMPLIFY", "AMIGA 8287", "DC REMOVE", "FILTER", "COMPRESSOR"};
  const char* const label = st.op == EditOp::Amiga && st.period == kAmigaPeriod16 ? "AMIGA 16574" : kNames[static_cast<unsigned>(st.op)];
  if (!samples_->edit_job().begin(spec, pcm.data, pcm.frames, dst, label)) {
    return;
  }
  const int job = session_.jobs.start(samples_->edit_job());
  if (job < 0) {
    say(core::Severity::Warn, "TOO MANY JOBS");
    return;
  }
  edit_slot_ = job;
  edit_target_ = slot;
  edit_version_ = samples_->version(slot);
  edit_dst_ = dst;
  edit_op_ = st.op;
  st.running = true;
  dirty_ = true;  // the key row reads CANCEL
  shown_percent_[job] = -1;
}

// The effect ended. A good result for the sample it started on is staged and committed by the internal action sample.commit (one undo unit, one history step);
// one for a sample that changed meanwhile, a failed or cancelled job, or a normalise with nothing to do, leaves the audio as it was. True when the strip has said there was nothing to do
// (FQA-10b2: the job's DONE would otherwise replace it; false when the queue refused the notice, then DONE says the job ended).
bool App::finish_edit(core::SlotState state) noexcept {
  if (state != core::SlotState::Done || samples_ == nullptr || samples_->version(edit_target_) != edit_version_) {
    return false;
  }
  if (samples_->edit_job().unchanged()) {
    // (upsert on the job's source: the notice rewrites the "EDIT NORMALISE nn%" entry at once; if the queue refuses it, false lets the job's DONE show)
    return state_.notify.upsert(core::Severity::Info, edit_op_ != EditOp::DcRemove ? "NOTHING TO NORMALISE" : (samples_->edit_job().held() ? "NO ROOM: SAMPLE AT FULL SCALE" : "NO DC OFFSET TO REMOVE"), "job", ViewState::kMessageMs);
  }
  SampleEditState& st = state_.sedit;
  st.staged = edit_dst_;
  st.staged_frames = samples_->edit_job().result_frames();
  st.ready_op = edit_op_;
  st.ready_slot = static_cast<std::uint8_t>(edit_target_);
  st.ready_a = samples_->edit_job().spec().a;
  st.ready_b = samples_->edit_job().spec().b;
  st.ready_period = samples_->edit_job().spec().period;
  st.ready = true;
  (void)invoke(core::action_id("sample.commit"), 0);
  st.ready = false;
  if (edit_op_ == EditOp::DcRemove && samples_->edit_job().held()) {  // OS-SEDFX: only part of the offset fitted under full scale
    (void)state_.notify.post(core::Severity::Info, "DC PARTLY REMOVED: FULL SCALE LIMIT", "edit", ViewState::kMessageMs);
  }
  return false;
}

// The waveform of the sample in view (Sample load's card, Wave / Chop, Sample edit, Params' source visual): the peak cache is built for it when it is not the cache's already, as a job
// ("PEAKS BREAK_A 40%"). A build for another sample is given up first. An import in progress builds its own (finish_import chains it).
void App::poll_peaks() noexcept {
  const bool after_batch = peaks_after_batch_;  // (taken once: only the pass that follows the batch's end)
  peaks_after_batch_ = false;
  if (samples_ == nullptr || import_slot_ >= 0 || samples_->peak_storage() == nullptr) {
    return;
  }
  if (state_.screen != ScreenId::SampleLoad && state_.screen != ScreenId::Wave && state_.screen != ScreenId::SampleEdit && state_.screen != ScreenId::Instrument) {
    return;
  }
  const unsigned slot = wave_slot(state_);
  const SampleBank::Pcm pcm = samples_->pcm(slot);
  const std::uint32_t version = samples_->version(slot);
  if (pcm.frames == 0u || (peaks_slot_ == slot && peaks_version_ == version)) {
    return;
  }
  if (peak_slot_ >= 0) {
    peaks_superseded_ = true;
    session_.jobs.cancel(static_cast<unsigned>(peak_slot_));  // released (silently) by the next report; started over after it
    return;
  }
  const model::Instrument* in = model_.project != nullptr ? model_.project->instrument(slot) : nullptr;
  core::FixedString<24> label;
  for (const char* p = in != nullptr && in->name[0] != '\0' ? in->name : "SAMPLE"; *p != '\0' && label.size() < 16; ++p) {
    const char c = (*p >= 'a' && *p <= 'z') ? static_cast<char>(*p - ('a' - 'A')) : *p;
    label.append(&c, 1);
  }
  peaks_quiet_ = state_.screen == ScreenId::Instrument || after_batch;  // (the Params visual's build says nothing on the strip; nor does the one after a multi-load)
  peaks_slot_ = slot;  // (also when it will not fit: not asked again every pass)
  peaks_version_ = version;
  if (!peak_job_.begin(peak_cache_, samples_->peak_storage(), samples_->peak_elems(), pcm.data, pcm.frames, 1, label.c_str())) {
    return;
  }
  const int job = session_.jobs.start(peak_job_);
  if (job >= 0) {
    peak_slot_ = job;
    shown_percent_[job] = -1;
  } else {
    peaks_slot_ = SampleBank::kSlots;  // no job slot free: the next pass asks again
  }
}

bool App::pad_lights(PadRole (&roles)[48]) const noexcept {
  if (state_.overlay == OverlayId::StepPopup) {  // OS-PADS-A: the pads that the held Instr / Fx / Note button's popup chooses from
    popup_roles(model_, state_, roles);
    return true;
  }
  if (state_.overlay == OverlayId::PatternChooser && state_.edit.chooser_held) {  // OS-PATSWITCH: F1 held, the pads are the patterns, as PATS lights them (a tap-opened chooser does not exist; LENGTH and JUMP stay dark)
    patterns_roles(state_, content_, blink_on(clock_.uptime_ms()), roles);
    return true;
  }
  if (state_.overlay != OverlayId::None) {
    return false;
  }
  if (state_.count_in.beat != 0u && static_cast<std::uint32_t>(clock_.uptime_ms()) - state_.count_in.at_ms < kFlashMs) {  // OS-METRO: the count-in's beat, every pad: Back, and Front on a bar
    for (PadRole& role : roles) {
      role = state_.count_in.bar ? PadRole::Front : PadRole::Back;
    }
    return true;
  }
  if (pads_tracks_in_force()) {  // OS-PADS-A
    tracks_roles(model_, state_, state_.screen == ScreenId::Perform, flash_.lit(), blink_on(clock_.uptime_ms()), roles);
    return true;
  }
  if (pads_mode_in_force()) {  // OS-PADS-B
    mode_roles(model_, state_, content_, blink_on(clock_.uptime_ms()), roles);
    return true;
  }
  if (!active_screen().pad_roles(model_, state_, roles)) {
    return false;
  }
  if (state_.dj_page()) {
    dj_pattern_roles(model_, state_, blink_on(clock_.uptime_ms()), roles);  // OS-DJ A4: the pattern row, the queued one blinking
  }
  return true;
}

// After the jobs: a listing that moved (entries came in, it was cleared) or a scan that changed state is a redraw when the screen shows it, and the
// cursor goes back onto its entry.
void App::follow_files() noexcept {
  const std::uint32_t generation = files_.index().generation();
  const DirScan::State st = files_.state();
  if (rescan_say_ && st != DirScan::State::Idle && st != DirScan::State::Scanning) {  // OS-QAFIX-RESCAN: the scan a RESCAN started has ended
    rescan_say_ = false;
    // (not told from another screen, nor over a question that is on the strip; a failed scan has its own words)
    if (st == DirScan::State::Done && ((state_.screen == ScreenId::File && !state_.file.asking()) || state_.screen == ScreenId::SampleLoad)) {
      core::FixedString<core::kNotifyTextChars> text;
      rescan_text(text, files_.index().count(), files_.truncated(), "ITEM", "ITEMS");
      (void)state_.notify.upsert(core::Severity::Info, text.c_str(), "file", kMessageMs);
    }
  }
  if (generation == files_seen_ && st == files_state_seen_) {
    return;
  }
  files_seen_ = generation;
  files_state_seen_ = st;
  if (files_for_ == FilesFor::Wavs) {
    (void)state_.wav.list.fit(files_.index());
    if (state_.wav.reselect[0] != '\0' && (st == DirScan::State::Done || st == DirScan::State::NoCard || st == DirScan::State::NoFolder || st == DirScan::State::Failed)) {
      // OS-AUDIT-A1 (E-LOAD-06, E-LOAD-23): the scan has ended; the cursor goes onto the remembered entry unless the user already moved it
      const int at = files_.index().find_name(state_.wav.reselect);
      if (at >= 0 && state_.wav.list.pos() == 0) {
        (void)state_.wav.list.set(at, files_.index());
      }
      state_.wav.reselect[0] = '\0';
    }
  } else {
    (void)state_.file.fit(files_.index());
  }
  if (state_.screen == ScreenId::File || state_.screen == ScreenId::SampleLoad) {
    dirty_ = true;
  }
}

// OS-FILES part B: the RECENT group above the PROJECTS list. It is shown once the listing is complete (a project that is not in it is not on the card as a project, and is dropped from the list at the
// next write; a truncated listing proves nothing, so none is dropped then), as the rows of the list's own entries, newest first. The group is rebuilt whenever it differs; the cursor stays on its entry.
void App::sync_recent() noexcept {
  const bool want = saves_ != nullptr && state_.screen == ScreenId::File && files_for_ == FilesFor::Projects && !state_.import.page && files_.state() == DirScan::State::Done;
  const DirIndex& index = files_.index();
  std::uint16_t keys[FileView::kMaxLead];
  unsigned n = 0;
  if (want) {
    const RecentList& recent = saves_->recent();
    char gone[RecentList::kMax][RecentList::kNameBytes];
    unsigned gone_n = 0;
    for (unsigned i = 0; i < recent.count() && n < FileView::kMaxLead; ++i) {
      const int at = index.find_name(recent.name(i));
      if (at >= 0 && index.is_dir(static_cast<unsigned>(at))) {
        keys[n++] = index.key(static_cast<unsigned>(at));
      } else if (!state_.file.pruned && !files_.truncated()) {
        std::memcpy(gone[gone_n++], recent.name(i), RecentList::kNameBytes);  // (only the first time the listing is complete: after that a name that is not in it may be a project saved since)
      }
    }
    for (unsigned i = 0; i < gone_n; ++i) {
      saves_->recent_forget(gone[i]);
    }
    state_.file.pruned = true;
  }
  const bool changed = state_.file.set_lead(want ? keys : nullptr, n, index);
  if (changed && state_.screen == ScreenId::File) {
    dirty_ = true;
  }
}

void App::render() noexcept {
  const model::Editor* doc = session_.document();
  model_.undo_depth = doc != nullptr ? doc->journal(model::UndoArea::Pattern).depth() : 0u;  // the info line's "undo n": the PATTERN area's depth
  ui::Renderer r(builder_, ui::theme(state_.theme));
  if (!r.begin(ui::Color::Pane)) {
    return;  // the sink is busy: stay dirty and try again on the next pass
  }
  render_scene(model_, state_, r);
  if (r.end()) {
    ++frames_;
  } else {
    ++failed_frames_;  // aborted by the builder: the same content would overflow again, so no retry loop
  }
  dirty_ = state_.notemap.pending();  // note-map pictures still to upload (song_notemap.h): another frame fills in the rest
}

}  // namespace o3os::app
