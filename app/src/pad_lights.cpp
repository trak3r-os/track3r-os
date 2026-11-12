// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/pad_lights.h>

#include "param_access.h"

#include <o3os/app/pattern_source.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/screens/overlay.h>

#include <cstring>

namespace o3os::app {

namespace {

// A slot "has content" when there is something to play or to talk to: a sample whose PCM is loaded (the project's path alone when there is no bank), a module, a MIDI channel.
bool instrument_has_content(const Model& model, unsigned slot) noexcept {
  const model::Instrument* in = model.project != nullptr ? model.project->instrument(slot) : nullptr;
  if (in == nullptr) {
    return false;
  }
  if (in->kind != model::InstrumentKind::Sample) {
    return true;
  }
  return model.samples != nullptr ? model.samples->pcm(slot).frames != 0u : in->sample_path[0] != '\0';
}

}  // namespace

void tracks_roles(const Model& model, const ViewState& view, bool perform, std::uint16_t flash, bool blink, PadRole (&roles)[48]) noexcept {
  const unsigned active = model.project != nullptr ? static_cast<unsigned>(model.project->active_tracks()) : 0u;
  const int cursor = view.screen == ScreenId::Song ? view.song.track() : view.pattern.track();  // (OS-SONGMAP: on Song row 1 is the map's track cursor, none on PAT)
  for (unsigned pad = 0; pad < kPadCount; ++pad) {
    const unsigned t = tracks_col(pad);
    PadRole role = PadRole::Off;
    if (perform && tracks_row(pad) == TracksRow::Track) {
      if (view.dj_page()) {  // OS-DJ A1: the groups, THRU Off, A Back, B Front (the screen names them)
        role = view.dj.group_of(t) == DjGroup::A ? PadRole::Back : (view.dj.group_of(t) == DjGroup::B ? PadRole::Front : PadRole::Off);
      } else {
        role = model.perform != nullptr && model.perform->bank.affected(t) ? PadRole::Front : PadRole::Off;
      }
    } else if (t < active) {
      const TrackLamp l = track_lamp(model, t);
      switch (tracks_row(pad)) {
        case TracksRow::Track:
          role = static_cast<int>(t) == cursor ? PadRole::Front : (((flash >> t) & 1u) != 0u ? PadRole::Back : PadRole::Off);
          break;
        case TracksRow::Mute:
          if (l.mute_pending) {
            role = blink ? PadRole::Front : PadRole::Off;
          } else {
            role = l.muted ? PadRole::Front : (l.solo_silenced ? PadRole::Back : PadRole::Off);
          }
          break;
        case TracksRow::MuteMode:
          role = l.mode == MuteMode::Pattern ? PadRole::Front : (l.mode == MuteMode::Bar ? PadRole::Back : PadRole::Off);
          break;
        case TracksRow::Solo:
          role = l.soloed ? PadRole::Front : PadRole::Off;
          break;
      }
    }
    roles[pad] = role;
  }
}

// STEP: the cursor track's steps, 48 to a page (pad_maps.h): Back = the step holds a note, Front = the playhead is on it (the track's own step: a shorter track wraps by itself), Off = empty
// or past the track's end.
void step_roles(const Model& model, const ViewState& view, PadRole (&roles)[48]) noexcept {
  const PatternSource src(model, view.edit.pattern);
  const int track = view.pattern.track();
  const int length = src.track_length(track);
  const int play = play_row(view.play, src.id());
  const int head = play >= 0 && length > 0 ? play % length : -1;
  for (unsigned pad = 0; pad < kPadCount; ++pad) {
    const int row = step_row(view.pattern.row(), pad);
    roles[pad] = row >= length ? PadRole::Off : (row == head ? PadRole::Front : (src.step(track, row).note != model::kNoteEmpty ? PadRole::Back : PadRole::Off));
  }
}

void PatternContent::poll(const model::Project* project) noexcept {
  if (project != project_) {
    project_ = project;
    mask_ = 0;
    seen_ = 0;
  }
  if (project == nullptr) {
    return;
  }
  unsigned budget = 2;
  for (unsigned n = 0; n < kPatterns; ++n) {
    const std::uint64_t bit = std::uint64_t{1} << n;
    const model::Pattern* p = project->pattern(n + 1u);
    const std::uint32_t gen = p != nullptr ? project->pattern_generation(n + 1u) : 0u;
    if ((seen_ & bit) != 0u && gen_[n] == gen && (p != nullptr || (mask_ & bit) == 0u)) {
      continue;
    }
    bool any = false;
    if (p != nullptr) {
      if (budget == 0u) {
        continue;  // the next pass
      }
      --budget;
      const unsigned tracks = project->active_tracks();
      for (unsigned t = 0; t < tracks && !any; ++t) {
        for (unsigned r = 0; r < p->track_length[t] && !any; ++r) {
          any = std::memcmp(&p->steps[t][r], &model::kEmptyStep, sizeof(model::Step)) != 0;
        }
      }
    }
    gen_[n] = gen;
    seen_ |= bit;
    mask_ = any ? (mask_ | bit) : (mask_ & ~bit);
  }
}

// PATS: the pads are patterns 1..48.
void patterns_roles(const ViewState& view, const PatternContent& content, bool blink, PadRole (&roles)[48]) noexcept {
  const unsigned playing = view.play.playing ? view.play.pattern : view.edit.pattern;
  for (unsigned pad = 0; pad < kPadCount; ++pad) {
    const unsigned id = pad + 1u;
    PadRole role = content.has(id) ? PadRole::Back : PadRole::Off;
    if (id == playing) {
      role = PadRole::Front;
    }
    if (id == view.edit.queued_pattern) {
      role = blink ? PadRole::Front : PadRole::Off;
    }
    roles[pad] = role;
  }
}

void dj_pattern_roles(const Model& model, const ViewState& view, bool blink, PadRole (&roles)[48]) noexcept {
  const unsigned now = view.dj_now();
  for (unsigned col = 0; col < kDjBankSize; ++col) {
    const unsigned id = view.dj_bank() * kDjBankSize + col + 1u;
    PadRole role = id <= 255u && model.project != nullptr && model.project->pattern(id) != nullptr ? PadRole::Back : PadRole::Off;
    if (id == now && id <= 255u) {
      role = PadRole::Front;
    }
    if (id == view.edit.queued_pattern && id <= 255u) {
      role = blink ? PadRole::Front : PadRole::Off;
    }
    roles[col] = role;
  }
}

// VU and FADERS: column c is track c (the project's active ones, at most the 12 columns); `lit(c)` is how many of its four pads are lit, from the bottom.
template <class Lit>
void bars(const Model& model, Lit lit, PadRole (&roles)[48]) noexcept {
  const unsigned tracks = model.project != nullptr ? static_cast<unsigned>(model.project->active_tracks()) : 0u;
  for (unsigned col = 0; col < kPadCols; ++col) {
    const unsigned n = col < tracks ? lit(col) : 0u;
    for (unsigned row = 0; row < 4u; ++row) {
      const unsigned from_bottom = 3u - row;
      roles[row * kPadCols + col] = from_bottom + 1u == n ? PadRole::Front : (from_bottom < n ? PadRole::Back : PadRole::Off);
    }
  }
}

void mode_roles(const Model& model, const ViewState& view, const PatternContent& content, bool blink, PadRole (&roles)[48]) noexcept {
  if (view.padmode.fixed() == PadMode::Vu) {
    if (view.padmode.faders()) {
      bars(model, [&](unsigned col) {
        float level = 0.0f;
        (void)read_param(model, core::param_id(core::ParamKind::Track, col, model::track_param::Volume), level);
        const unsigned n = level > 0.0f ? (static_cast<unsigned>(level) + 24u) / 25u : 0u;
        return n < 4u ? n : 4u;
      }, roles);
    } else {
      const MixBoard* mix = model.mix;
      bars(model, [&](unsigned col) {
        if (mix == nullptr) {
          return 0u;
        }
        const unsigned n = (static_cast<unsigned>(mix->scale.position(mix->meter[audio::meter_ch::kTrack0 + col].peak())) + 16383u) / 16384u;
        return n < 4u ? n : 4u;
      }, roles);
    }
    return;
  }
  if (view.padmode.fixed() == PadMode::Steps) {
    step_roles(model, view, roles);
    return;
  }
  if (view.padmode.fixed() == PadMode::Patterns) {
    patterns_roles(view, content, blink, roles);
    return;
  }
  const NoteEntry& e = view.edit.entry;
  if (view.padmode.fixed() == PadMode::Notes) {  // OS-NOTESLED: the NOTES board lit as PIANO and ISO light their notes (nothing else on the pads: no jump, no octave pads)
    for (unsigned pad = 0; pad < kPadCount; ++pad) {
      const unsigned n = view.edit.pads.note(pad);
      roles[pad] = n == kNoNote ? PadRole::Off : (e.in_scale(n) ? PadRole::Front : PadRole::Back);  // OS-NOTESLED2: scale notes HIGH, the others LOW (with SCALE LOCK or no scale: all Front)
    }
    return;
  }
  for (unsigned pad = 0; pad < kPadCount; ++pad) {
    PadFn fn;
    switch (view.padmode.fixed()) {
      case PadMode::Piano:
        fn = piano_pad(pad);
        break;
      case PadMode::Iso:
        fn = iso_pad(pad);
        break;
      default:
        break;
    }
    PadRole role = PadRole::Off;
    switch (fn.kind) {
      case PadKey::Note: {
        const unsigned n = pad_fn_note(e, fn);
        // OS-NOTESLED2: with SCALE LOCK the pad plays the locked note (pad_mode_actions.cpp), so every note pad is in the scale
        role = n == kNoNote ? PadRole::Off : (e.lock != 0u || e.in_scale(n) ? PadRole::Front : PadRole::Back);
        break;
      }
      case PadKey::Nav:
        role = PadRole::Back;
        break;
      case PadKey::Bend:  // OS-PIANOBEND: dim at rest, bright while the finger is on it
        role = view.edit.bend.holds(pad) ? PadRole::Front : PadRole::Back;
        break;
      case PadKey::OctUp:
        // OS-NOTESLED2: Front while the octave is shifted up from the default, Back otherwise, Off at the end of the range
        role = e.octave + 1u >= kEntryOctaves ? PadRole::Off : (e.octave > kEntryDefaultOctave ? PadRole::Front : PadRole::Back);
        break;
      case PadKey::OctDown:
        role = e.octave == 0u ? PadRole::Off : (e.octave < kEntryDefaultOctave ? PadRole::Front : PadRole::Back);
        break;
      default:
        break;
    }
    roles[pad] = role;
  }
}

void popup_roles(const Model& model, const ViewState& view, PadRole (&roles)[48]) noexcept {
  for (unsigned pad = 0; pad < kPadCount; ++pad) {
    roles[pad] = PadRole::Off;
  }
  switch (view.popup.kind) {
    case StepPopupView::Instr:
      for (unsigned pad = 0; pad < kPadCount; ++pad) {  // pad n is instrument n + 1 (popup_pad)
        roles[pad] = instrument_has_content(model, pad) && step_popup_pickable(model, view, pad) ? PadRole::Back : PadRole::Off;
      }
      if (view.instrument >= 1 && static_cast<unsigned>(view.instrument) <= kPadCount) {
        roles[view.instrument - 1] = PadRole::Front;
      }
      break;
    case StepPopupView::Fx:
      if (view.popup.item >= 0 && static_cast<unsigned>(view.popup.item) < kPadCount) {
        roles[view.popup.item] = PadRole::Front;
      }
      break;
    default: {  // Note (OS-PADS-B): the settings. Every pad of a row Back, the row's choice Front: ROOT, SCALE, JUMP (0..11), OCTAVE (0..9) and the two SCALE LOCK pads
      const NoteEntry& e = view.edit.entry;
      // OS-SCALES28: row 2's Front pad is the pad whose scale is in force; one of the 27 outside the twelve lights no pad of the row, as JUMP above 11 leaves row 3 dark
      unsigned scale_col = 12u;
      for (unsigned c = 0; c < 12u; ++c) {
        if (model::kNotePanelScales[c] == e.scale) {
          scale_col = c;
          break;
        }
      }
      const unsigned choice[4] = {e.root, scale_col, view.edit.edit_step, e.octave};
      for (unsigned pad = 0; pad < kPadCount; ++pad) {
        const unsigned row = pad / kPadCols;
        const unsigned col = pad % kPadCols;
        const bool lock = row == 3u && col >= 10u;
        const unsigned want = lock ? 10u + (e.lock != 0u ? 1u : 0u) : choice[row];
        roles[pad] = col == want ? PadRole::Front : PadRole::Back;
      }
      break;
    }
  }
}

}  // namespace o3os::app
