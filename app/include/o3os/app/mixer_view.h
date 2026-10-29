// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Where the cursor of the Mixer screen is and what it points at (OS-011b): pure functions of the model and the view, shared by the screen (which draws it), its actions (which
// edit it) and the tests. The screen has four pages (Master pressed again goes to the next); the cursor is two numbers kept in the view's generated-page cursor (ParamView:
// `row` and `env`), so the screen costs the main loop's SRAM nothing:
//
//   MIXER     env = the column: T1..Tn (the active tracks), then IN, SND A, SND B, CUE, MASTER (MixBus); row = the field of a track column: 0 LEVEL, 1 PAN (the buses have LEVEL only)
//   ROUTING   row = the source: T1..Tn, LINE IN, RET A, RET B, OUT MODE; env = the destination: MAIN, CUE, SND A, SND B, REC, USB (a return has MAIN and CUE only; OUT MODE has none)
//   MASTER FX row = the master section's eight parameters in table order (VOLUME, DRY, REVERB, DELAY, the three mutes, LIMITER), CUE LEVEL, then the twelve controls of the send effects (OS-SENDFX) and the limiter's three (OS-LIMIT)
//   STEMS     row 0 = OUTPUTS (OS-USBOUTS: the number of USB outputs, a pending value applied by F6 twice), row n = the USB slot Sn (S1..), env = a source of the slot (T1..T12, ML, MR, SA, SB)

#include <o3os/app/mix_board.h>
#include <o3os/app/model.h>
#include <o3os/app/view_state.h>
#include <o3os/core/param.h>
#include <o3os/model/params.h>

#include <cstdint>

namespace o3os::app {

enum class MixerPage : std::uint8_t { Mixer = 0, Routing = 1, MasterFx = 2, Stems = 3 };
inline constexpr unsigned kMixerPages = 4;

// Eight labels for F1..F8 (docs/design/product-ux.md section 4, "Mixer and routing"): F1..F4 are the pages, F5 the cue level, F6 is the mock's PRESET (no presets exist yet),
// F7 mutes and F8 solos what the cursor is on.
inline constexpr const char* kMixerSoftKeys[8] = {"MIXER", "ROUTING", "MST FX", "STEMS", "CUE LVL", "--", "MUTE", "SOLO"};
// On the STEMS page F6 is APPLY: the pending OUTPUTS value (OS-USBOUTS), pressed twice because the port re-enumerates.
inline constexpr const char* kMixerStemKeys[8] = {"MIXER", "ROUTING", "MST FX", "STEMS", "CUE LVL", "APPLY", "MUTE", "SOLO"};
// While Shift is held (OS-SHIFTROW): F7 UNMUTE (every strip), F8 UNSOLO (every strip): mixer.mute_none / mixer.solo_none, one undo unit each. Not REDO: F8 is SOLO here. F1 NAME (OS-NAMES): the track's name.
inline constexpr const char* kMixerShiftKeys[8] = {"NAME", "-", "-", "-", "-", "-", "UNMUTE", "UNSOLO"};

inline constexpr unsigned kStemSources = 16;  // the columns of a stem slot (audio::stem_src: T1..T12, ML, MR, SA, SB)
// OS-SMALL3: the LIMITER switch (the model's last master parameter) is not a row: the limiter is always on (OS-LIMIT) and the card says so (OS-MIXHIDE); the stored value, the table and the OG import are untouched.
static_assert(model::master_param::Limiter + 1u == model::master_param::kCount, "the hidden LIMITER parameter is the last of the master section");
inline constexpr unsigned kMasterShownParams = model::master_param::Limiter;  // the master parameters the page lists, in table order
inline constexpr unsigned kMasterCueRow = kMasterShownParams;
inline constexpr unsigned kMasterFirstFxRow = kMasterCueRow + 1u;                       // OS-SENDFX: then the send effects' controls (send_fx_rows.h), the delay's and the reverb's
inline constexpr unsigned kMasterFirstLimRow = kMasterFirstFxRow + kSendFxRows;         // OS-LIMIT: after the send effects' rows, the limiter's THRESHOLD, ATTACK and RELEASE (limiter_rows.h)
inline constexpr unsigned kMasterFxRows = kMasterFirstFxRow + kMasterFxControls;        // the master's parameters, CUE LEVEL, the send effects' rows and the limiter's (more than the pane shows: the page scrolls)

namespace mixer {

inline MixerPage page_of(const ViewState& view) noexcept {
  const unsigned p = view.page[screen_index(ScreenId::Mixer)];
  return static_cast<MixerPage>(p < kMixerPages ? p : 0u);
}

// The tracks the pages list: the project's active ones, or the default twelve without a project.
inline unsigned tracks(const Model& model) noexcept {
  const unsigned n = model.project != nullptr ? model.project->active_tracks() : kMixDefaultTracks;
  return n < 1u ? 1u : (n > kMixTracks ? kMixTracks : n);
}

// Columns of the MIXER page (tracks and buses).
inline unsigned columns(const Model& model) noexcept { return tracks(model) + kMixBuses; }
inline bool is_track_column(const Model& model, unsigned column) noexcept { return column < tracks(model); }
// The bus a column is, kMixBuses when it is a track's.
inline unsigned bus_of(const Model& model, unsigned column) noexcept {
  const unsigned t = tracks(model);
  return column >= t && column < t + kMixBuses ? column - t : kMixBuses;
}

// The USB slots the STEMS page lists: the stem map's channels, 0 without a map.
inline unsigned stem_rows(const Model& model) noexcept {
  return model.mix != nullptr && model.mix->stems != nullptr ? model.mix->stems->channels() : 0u;
}

// OS-USBOUTS: the numbers of outputs the OUTPUTS row offers (profile 11 is console-only and not offered).
inline constexpr std::uint8_t kOutputChoices[5] = {2, 4, 6, 8, 10};
// The next offered value from `base` in the direction of `delta` (the nearest above for delta > 0, below for delta < 0); `base` itself when there is none.
inline std::uint8_t outputs_next(std::uint8_t base, int delta) noexcept {
  std::uint8_t best = base;
  for (const std::uint8_t c : kOutputChoices) {
    if (delta > 0 && c > base && (best == base || c < best)) {
      best = c;
    } else if (delta < 0 && c < base && (best == base || c > best)) {
      best = c;
    }
  }
  return best;
}
// The usb::Profile of an offered value.
inline usb::Profile outputs_profile(std::uint8_t channels) noexcept {
  switch (channels) {
    case 2: return usb::Profile::P2;
    case 4: return usb::Profile::P4;
    case 6: return usb::Profile::P6;
    case 8: return usb::Profile::P8;
    default: return usb::Profile::P10;
  }
}

// Rows and columns of the cursor on a page. `rows` is the length of the list the up / down arrows walk; `cols` the length of the other axis.
inline unsigned rows(const Model& model, const ViewState& view) noexcept {
  switch (page_of(view)) {
    case MixerPage::Mixer: return 2;                      // LEVEL, PAN
    case MixerPage::Routing: return tracks(model) + 4u;   // T1..Tn, LINE IN, RET A, RET B, OUT MODE
    case MixerPage::MasterFx: return kMasterFxRows;
    case MixerPage::Stems: return stem_rows(model) < 1u ? 1u : stem_rows(model) + 1u;  // the OUTPUTS row, then the slots (none without a map)
  }
  return 1;
}

struct Cursor {
  unsigned row;
  unsigned col;
};

// The cursor held inside the page: whatever the view says (a project that has fewer tracks, a page that has another shape).
inline Cursor cursor(const Model& model, const ViewState& view) noexcept {
  const MixerPage page = page_of(view);
  unsigned row_max = rows(model, view);
  unsigned col_max = 1;
  switch (page) {
    case MixerPage::Mixer: col_max = columns(model); break;
    case MixerPage::Routing: col_max = kRouteCols; break;
    case MixerPage::MasterFx: col_max = 1; break;
    case MixerPage::Stems: col_max = kStemSources; break;
  }
  const unsigned row = view.mixer.row;
  const unsigned col = view.mixer.col;
  Cursor c{row >= row_max ? row_max - 1u : row, col >= col_max ? col_max - 1u : col};
  if (page == MixerPage::Mixer && bus_of(model, c.col) != kMixBuses) {
    c.row = 0;  // a bus has a level and no pan
  }
  if (page == MixerPage::Routing && c.row >= tracks(model) + 1u) {
    // RET A, RET B reach MAIN and CUE only; OUT MODE has no column
    const unsigned extra = c.row - tracks(model) - 1u;
    if (extra < 2u && c.col >= 2u) {
      c.col = 1;
    } else if (extra == 2u) {
      c.col = 0;
    }
  }
  return c;
}

// The parameter under the cursor: what `param.edit` changes and MIDI Learn binds. ParamId::None where the value is the board's (the INPUT and CUE levels, the routing, the stems)
// or the cursor is on nothing editable.
inline core::ParamId focus(const Model& model, const ViewState& view) noexcept {
  const Cursor c = cursor(model, view);
  using core::param_id;
  using core::ParamKind;
  switch (page_of(view)) {
    case MixerPage::Mixer: {
      if (is_track_column(model, c.col)) {
        return param_id(ParamKind::Track, c.col, c.row == 0u ? model::track_param::Volume : model::track_param::Pan);
      }
      switch (static_cast<MixBus>(bus_of(model, c.col))) {
        case MixBus::SndA: return param_id(ParamKind::Master, 0, model::master_param::Delay);
        case MixBus::SndB: return param_id(ParamKind::Master, 0, model::master_param::Reverb);
        case MixBus::Master: return param_id(ParamKind::Master, 0, model::master_param::Volume);
        default: return core::ParamId::None;
      }
    }
    case MixerPage::MasterFx:
      return c.row < kMasterShownParams ? param_id(ParamKind::Master, 0, c.row) : core::ParamId::None;
    case MixerPage::Routing:
    case MixerPage::Stems:
      break;
  }
  return core::ParamId::None;
}

}  // namespace mixer

}  // namespace o3os::app
