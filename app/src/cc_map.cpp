// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-MIDICCMAP (docs/specs/OS-MIDICCMAP.md, sections 3.2 and 3.3): the app's side of the fixed CC map. midi_cc (app/src/param_actions.cpp) calls cc_map_apply after both
// Learn stores, when CC MAP is On and no binding of either store listens to the message (Q2 a). S2 is the mixer's CC 71..82, on every screen; S3 is the Perform slots
// CC 51..62, on the Perform screen's slot page only (Q5 a); S4 is the Perform tracks CC 41..48, on the same page and only while the sequencer plays. Main loop only, an action
// like the rest of midi.cc: nothing in the render or an interrupt changes, no allocation.
#include <o3os/app/cc_map.h>

#include "action_handlers.h"
#include "param_access.h"

#include <o3os/app/mix_board.h>
#include <o3os/app/mixer_actions.h>
#include <o3os/app/perform_board.h>
#include <o3os/app/text_format.h>
#include <o3os/audio/graph_params.h>
#include <o3os/core/param.h>
#include <o3os/model/params.h>

namespace o3os::app {

// One message of the map. True when something changed.
bool cc_map_apply(ActionContext& ctx, unsigned cc, unsigned value) noexcept {
  const CcTarget t = cc_map_target(cc);
  const unsigned level = cc_map_level(value);
  switch (t.kind) {
    case CcMapKind::Track:
    case CcMapKind::MasterDelay:
    case CcMapKind::MasterReverb:
    case CcMapKind::MasterDry: {
      if (ctx.edit == nullptr) {
        return false;  // A-6: the mixer writes need a project
      }
      const unsigned field = t.kind == CcMapKind::Track ? model::track_param::Volume
                                  : t.kind == CcMapKind::MasterDelay ? model::master_param::Delay
                                  : t.kind == CcMapKind::MasterReverb ? model::master_param::Reverb
                                                                      : model::master_param::Dry;
      const core::ParamId id = t.kind == CcMapKind::Track ? core::param_id(core::ParamKind::Track, t.index, field)
                                                          : core::param_id(core::ParamKind::Master, 0, field);
      // D10: a track's or the master's parameter is final, no history, as apply_cc does for a mixer parameter. A mapped CC is not a Learn write, so it records no FX step
      // in an open take either (OS-FXREC): only the parameter moves.
      const bool was_final = ctx.edit->final_edits();
      ctx.edit->set_final(true);
      const WriteResult r = write_param(ctx, id, static_cast<float>(level));
      ctx.edit->set_final(was_final);
      return r == WriteResult::Changed;
    }
    case CcMapKind::Input: {
      if (ctx.mix == nullptr) {
        return false;
      }
      // A-3: CC 82 is our INPUT strip's level (the official line-in volume): the board's, told to the engine as the strip's Volume, as the Mixer's encoder does. No project needed.
      return set_level(ctx, ctx.mix->in_level, level, audio::kInputStrip, false, audio::graph_param::strip::Volume);
    }
    case CcMapKind::PerfSlot: {
      // Q5 a / A-4: only while the Perform screen's slot page is in view (not the DJ page, not another screen). The bank holds the rules (3.4): CC mode, the value law,
      // exclude, apply to the affected tracks. A slot with no FX ignores it there.
      if (!on_perform(ctx) || ctx.perform == nullptr) {
        return false;
      }
      return ctx.perform->bank.set_cc(t.index, value, ctx.perform->sink());
    }
    case CcMapKind::PerfTrack: {
      // Q5 a / A-5 / F12: only while the Perform screen's slot page is in view and the sequencer plays (a queue is ignored while stopped: no strip message, a fader would flood it).
      // The track CCs ignore the affected mask: the official handler addresses the track by its number. Pattern v + 1, loaded at the track's next switch (E-PERF-07's runtime queue,
      // no project change, A-5). A pattern the project does not have is refused with the map's one strip message (A-8). The command is sent only when the value differs from the last
      // one this map sent for that track (PerformBoard::cc_pattern), so a fader resending the same value never takes the queue back (F12).
      if (!on_perform(ctx) || ctx.perform == nullptr || !ctx.view.play.playing) {
        return false;
      }
      const unsigned pattern = cc_map_pattern(value);
      PerformBoard& board = *ctx.perform;
      if (board.cc_pattern[t.index] == pattern) {
        return false;  // the same value again: nothing sent, the queue is not taken back
      }
      if (ctx.model.project == nullptr || ctx.model.project->pattern(pattern) == nullptr) {
        core::FixedString<core::kNotifyTextChars> text;
        text.assign("CC ");
        (void)append_dec(text, cc, 2u);
        text.append(": NO PAT ");
        (void)append_dec(text, pattern, 3u);
        (void)say_tag(ctx, core::Severity::Info, text.c_str(), "midi");
        return false;
      }
      const std::uint32_t mask = std::uint32_t{1} << t.index;  // the one track's bit
      if (!board.sink().push(seq::Command{seq::Command::Kind::QueueTrackPattern, 0, static_cast<std::uint16_t>(pattern), mask})) {
        return false;  // the ring is full: nothing was sent, so the value is not recorded
      }
      board.cc_pattern[t.index] = static_cast<std::uint8_t>(pattern);
      return true;
    }
    case CcMapKind::None:
    default:
      return false;  // not in the map
  }
}

}  // namespace o3os::app
