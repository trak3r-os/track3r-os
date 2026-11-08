// SPDX-License-Identifier: GPL-3.0-or-later
// The Preview key of the sample screens (OS-FU3). On Sample Playback it is F1: note 60 at the instrument's velocity, on at the press and off at the release, and the
// screen's progress cursor follows (a note on); the Sample Recorder's preview plays the take once recording has stopped
// (note 60 at velocity 100). F1 is not free on the Community OS screens (Wave / Chop's F1 is START, Sample edit's an
// effect, Record's is REC/STOP), so the key is F6 on Wave / Chop, F7 on Sample edit and F8 on Record (the soft-key labels say PREVIEW).
//
// The action only records the request: the key and what to play (ViewState::hold); App::poll_hold starts the sound once the engine can take it and the key's release, a raw event, ends it.
// NEW OS BEHAVIOUR: Shift + Preview plays from where the screen is (the selected point or slice when it lies in the zoom window, else the window's left edge; on Sample edit the
// selection's edge being moved, else the window's left edge) instead of from the instrument's start point; it is a one shot of the sample from there to its end (no loop, envelope or filter).
#include "action_handlers.h"

#include <o3os/app/recorder.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/screens/sample_edit_screen.h>
#include <o3os/app/screens/wave_screen.h>

namespace o3os::app {

namespace {

constexpr std::uint8_t kSlice = 4;
constexpr std::uint8_t kBeatSlice = 5;

bool say(ActionContext& ctx, const char* text) noexcept { return say_tag(ctx, core::Severity::Info, text, "preview"); }

// The frame Shift + Preview starts at, in the sample of `frames`.
std::uint32_t cursor_frame(const ActionContext& ctx, const model::Instrument& in, std::uint32_t frames) noexcept {
  const ViewState& v = ctx.view;
  std::uint32_t at = 0;
  Window w;
  if (v.screen == ScreenId::Wave) {
    w = wave_window(v.wave, frames);
    if (in.sampler.play_mode >= 6u) {  // OS-INSTUI S4: a wavetable and a granular instrument have no point or slice to start from (the cursor there is a row of the page): the window's edge
      return w.first;
    }
    if (v.page[screen_index(ScreenId::Wave)] == 1u) {  // CHOP: the selected slice
      const unsigned count = in.sampler.slice_count > kMaxSliceCount ? kMaxSliceCount : in.sampler.slice_count;
      at = v.wave.slice < count ? point_to_frame(in.sampler.slices[v.wave.slice], frames) : w.first;
    } else {
      const std::uint16_t points[4] = {in.sampler.start, in.sampler.loop_start, in.sampler.loop_end, in.sampler.end};
      at = point_to_frame(points[v.wave.point & 3u], frames);
    }
  } else {
    const SampleEditState& st = v.sedit;
    w = sedit_window(st, frames);
    at = st.a != st.b ? (st.edge == 0u ? st.a : st.b) : w.first;
  }
  return at >= w.first && at < w.first + w.span && at < frames ? at : w.first;
}

}  // namespace

// arg = the key's Button id | 0x100 for Shift + Preview (from where the screen is).
bool preview_hold(ActionContext& ctx, core::ActionArg arg) noexcept {
  ViewState& v = ctx.view;
  if ((v.screen != ScreenId::Wave && v.screen != ScreenId::SampleEdit) || arg < 0) {
    return false;
  }
  const model::Project* project = ctx.model.project;
  const unsigned slot = wave_slot(v);
  const model::Instrument* in = project != nullptr ? project->instrument(slot) : nullptr;
  const SampleBank::Pcm pcm = ctx.model.samples != nullptr ? ctx.model.samples->pcm(slot) : SampleBank::Pcm{};
  if (in == nullptr || in->kind != model::InstrumentKind::Sample || pcm.frames == 0u) {
    return say(ctx, "NO SAMPLE IN THIS INSTRUMENT");
  }
  if (in->sampler.play_mode == kSlice || in->sampler.play_mode == kBeatSlice) {
    return say(ctx, "THE PADS PLAY THE SLICES");  // the pads sound the slices in these modes
  }
  HeldPreview& h = v.hold;
  h.button = static_cast<std::uint8_t>(arg & 0xFF);
  h.pending = true;
  if ((arg & 0x100) != 0) {
    h.kind = HeldPreview::Over;
    h.from = cursor_frame(ctx, *in, pcm.frames);
  } else {
    h.kind = HeldPreview::Start;
    h.from = 0;
  }
  return true;
}

// F8 of the Record screen once a take exists: its trim window plays while the key is down.
bool record_preview(ActionContext& ctx, core::ActionArg arg) noexcept {
  const Recorder* rec = ctx.model.rec;
  if (ctx.view.screen != ScreenId::Record || rec == nullptr || arg < 0) {
    return false;
  }
  if (rec->loop_review() && ctx.rec != nullptr) {  // OS-LOOPER: under a loop's review the key is the loop's switch (a toggle, named by its state), not the held preview; the pads still play the take
    ctx.rec->toggle_loop_mute();
    return true;
  }
  if (rec->state() != Recorder::State::Review || rec->take_data() == nullptr || rec->trim_end() <= rec->trim_start()) {
    return say(ctx, "NO TAKE TO PREVIEW");
  }
  HeldPreview& h = ctx.view.hold;
  h.button = static_cast<std::uint8_t>(arg & 0xFF);
  h.kind = HeldPreview::Take;
  h.from = rec->trim_start();
  h.pending = true;
  return true;
}

}  // namespace o3os::app
