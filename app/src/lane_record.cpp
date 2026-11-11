// SPDX-License-Identifier: GPL-3.0-or-later
// OS-MIDIREC-P1 S3: the main loop's side of a Lane recording (see app/lane_record.h and the spec's "S3 as built").
#include <o3os/app/lane_record.h>

#include <o3os/app/midi_input.h>
#include <o3os/app/model.h>
#include <o3os/app/screens/pattern_screen.h>
#include <o3os/app/view_state.h>
#include <o3os/midi/cc_in.h>
#include <o3os/model/editor.h>
#include <o3os/model/event_list.h>
#include <o3os/model/lane_take.h>
#include <o3os/model/params.h>
#include <o3os/model/project.h>
#include <o3os/seq/midi_route.h>

namespace o3os::app {
namespace {

using model::Status;

// Where the end tick of a take may differ from the last stamped tick by a block's worth of stale playhead (the stamp is taken in the render, the position a block or two older): a playhead this
// close below the last message is that, not a loop wrap (a wrap puts the playhead far below it).
constexpr std::uint32_t kStaleTicks = 8;

// upsert: the newest word of the recording is the one on the strip (a refusal that was kept, then dropped, says so)
void say(ViewState& view, core::Severity sev, const char* text) noexcept { (void)view.notify.upsert(sev, text, "lane_rec", ViewState::kMessageMs); }

// A MIDI instrument under the cursor and REC STORE = Lane. `route` is the instrument's channel and port.
bool midi_lane(const Model& model, const ViewState& view, std::uint8_t& track, seq::MidiRoute& route) noexcept {
  if (model.project == nullptr || model.device.kind() != core::ParamKind::Device || model.device.get(model::device_setting::RecStore) < 1.0f) {
    return false;
  }
  const unsigned idx = static_cast<unsigned>(view.instrument);
  const model::Instrument* in = idx >= 1u ? model.project->instrument(idx - 1u) : nullptr;
  const unsigned t = static_cast<unsigned>(view.pattern.track());
  if (in == nullptr || in->kind != model::InstrumentKind::Midi || t >= model::kMaxTracks) {
    return false;
  }
  track = static_cast<std::uint8_t>(t);
  route = seq::midi_route(*in);
  return true;
}

// ... and the Pattern screen recording in LIVE.
bool lane_wanted(const Model& model, const ViewState& view, std::uint8_t& track, seq::MidiRoute& route) noexcept {
  return pattern_recording(model, view) && midi_lane(model, view, track, route);
}

// ... and the count-in of such a recording (the take is armed from it: decision (b) holds from the arm).
bool lane_counting_in(const Model& model, const ViewState& view) noexcept {
  std::uint8_t track = 0;
  seq::MidiRoute route{};
  return pattern_counting_in(model, view) && midi_lane(model, view, track, route);
}

// ... and the Pattern screen in STEP (Edit with the cursor free), a single cell selected (a block takes no note, as in the grid).
bool step_wanted(const Model& model, const ViewState& view, std::uint8_t& track, seq::MidiRoute& route) noexcept {
  if (view.screen != ScreenId::Pattern || view.mode[screen_index(ScreenId::Pattern)] != static_cast<std::uint8_t>(PatternMode::Edit) || !view.edit.rec_free || !midi_lane(model, view, track, route)) {
    return false;
  }
  const Selection::Block b = view.edit.selection.block(view.pattern.row(), view.pattern.track());
  return b.row0 == b.row1 && b.track0 == b.track1;
}

// The pattern the track is playing: its Perform override when it has one (the same source as the stamps' pattern), otherwise the song's.
std::uint8_t track_pattern(const ViewState& view, std::uint32_t track) noexcept {
  const std::uint8_t over = track < audio::PlayPosition::kTrackWords ? static_cast<std::uint8_t>(view.track_pat[track] & 0xFFu) : 0u;
  return over != 0u ? over : view.play.pattern;
}

// The playhead's tick on the armed track: the song cursor's step folded into the track's own length (a track shorter than the pattern loops inside it), 0..6143.
std::uint32_t playhead_tick(const Model& model, const ViewState& view, std::uint32_t track) noexcept {
  const model::Pattern* p = model.project != nullptr ? model.project->pattern(track_pattern(view, track)) : nullptr;
  const std::uint32_t len = p != nullptr && p->track_length[track] != 0u ? p->track_length[track] : 1u;
  return (static_cast<std::uint32_t>(view.play.step) % len) * model::kLaneTicksPerStep + view.play.ustep;
}

// Merge the finished take and say what did not go. `keep`: a refusal for lack of room keeps the take for one more try (rec.retry), otherwise it is dropped.
void commit(LaneRec& r, model::Editor* editor, ViewState& view, bool keep) noexcept {
  if (r.take->count() == 0u) {
    return;
  }
  const Status st = editor != nullptr ? editor->lane_merge_take(*r.take) : Status::NotFound;
  if (st == Status::Ok) {
    r.retry = false;
    return;
  }
  if (st == Status::Full && keep) {
    r.retry = true;
    say(view, core::Severity::Warn, "LANE TAKE: NO ROOM, KEPT");
    return;
  }
  r.retry = false;
  say(view, core::Severity::Warn, st == Status::Full ? "LANE TAKE: NO ROOM, DROPPED" : "LANE TAKE LOST");
}

void finish_take(LaneRec& r, model::Editor* editor, ViewState& view, std::uint32_t end, bool keep) noexcept {
  if (r.take->open()) {
    r.take->finish(end);
    commit(r, editor, view, keep);
  }
}

// The end tick of the take: the playhead, unless it is a block's staleness below the last message.
std::uint32_t end_tick(const LaneRec& r) noexcept {
  return r.last_pos < r.last_tick && r.last_tick - r.last_pos <= kStaleTicks ? r.last_tick : r.last_pos;
}

// STEP: the chord is over (the last key is up, or STEP ended): merge it; `advance` moves the cursor on by the edit step, wrapping at the pattern's end as an entry of the grid does.
void step_end(LaneRec& r, model::Editor* editor, const Model& model, ViewState& view, bool advance) noexcept {
  if (!r.step_open) {
    return;
  }
  r.step_open = false;
  r.step_n = 0;
  for (std::uint32_t& w : r.step_held) {
    w = 0;
  }
  finish_take(r, editor, view, r.step_tick, true);
  const int step = view.edit.edit_step;
  if (advance && step > 0) {
    const PatternView::Dims d = pattern_dims(model, view);
    if (d.steps > 0) {
      (void)view.pattern.jump_row((view.pattern.row() + step) % d.steps, d);
      view.edit.selection.clear();
    }
  }
}

void feed(LaneRec& r, model::Editor* editor, const Model& model, ViewState& view, const midi::StampMsg& m) noexcept {
  model::LaneTake& take = *r.take;
  if (take.open() && take.pattern() != m.pattern) {  // the song moved on to another pattern: the take of the old one ran to the end of it (the playhead's last tick is no stop position: it already follows the song)
    finish_take(r, editor, view, take.length_ticks() - 1u, false);
  }
  if (!take.open()) {
    const model::Pattern* p = model.project != nullptr ? model.project->pattern(m.pattern) : nullptr;
    if (p == nullptr || m.tick >= model::kLaneTicks) {
      return;
    }
    take.begin(m.pattern, r.track, r.channel, r.port, m.tick, static_cast<std::uint32_t>(p->track_length[r.track]) * model::kLaneTicksPerStep);
    r.pattern = m.pattern;
    r.last_pos = m.tick;  // a stop before the playhead is read (the count-in) ends at the first stamp, not at 0 (a false wrap)
  }
  bool stored = true;
  switch (m.kind) {
    case midi::StampKind::On:
      stored = take.note_on(m.tick, m.a, m.b);
      break;
    case midi::StampKind::Off:
      take.note_off(m.tick, m.a);
      break;
    case midi::StampKind::AllOff:
      take.all_notes_off(m.tick);
      break;
    case midi::StampKind::Cc:
      stored = take.cc(m.tick, m.a, m.b);
      break;
  }
  if (stored) {
    r.last_tick = m.tick;
    return;
  }
  // The buffer is full and this message did not fit: what it has goes into the lane now (a half-open range, so it ends before the message) and the message
  // starts a new take (a note held across the split ends there). An empty take stores any message, so the retry cannot recurse again.
  finish_take(r, editor, view, m.tick, false);
  say(view, core::Severity::Warn, "TAKE FULL: COMMITTED");
  feed(r, editor, model, view, m);
}

void drain(LaneRec& r, midi::CcInput& in, model::Editor* editor, const Model& model, ViewState& view, unsigned in_port, unsigned in_channel, unsigned max) noexcept {
  midi::StampMsg m;
  for (unsigned n = 0; n < max && in.pop_stamped(m); ++n) {
    if (midi_in_accepts(in_port, in_channel, m.port, m.channel)) {  // the keyboard's own filter (MIDI IN, IN CHANNEL), as the grid's note entry has it
      feed(r, editor, model, view, m);
    }
  }
  const std::uint32_t drops = in.stamp_dropped();
  if (drops != r.stamp_drops) {
    r.stamp_drops = drops;
    if (!r.said_drop) {
      r.said_drop = true;
      say(view, core::Severity::Warn, "MIDI IN OVERRUN: NOTES MISSED");
    }
  }
}

void stop(LaneRec& r, midi::CcInput& in, model::Editor* editor, const Model& model, ViewState& view, unsigned in_port, unsigned in_channel) noexcept {
  in.close_take();
  drain(r, in, editor, model, view, in_port, in_channel, 2u * midi::CcInput::kStampRingSize);
  finish_take(r, editor, view, end_tick(r), true);
  r.track = LaneRec::kNone;
  r.said_drop = false;
}

}  // namespace

bool lane_poll(LaneRec& r, midi::CcInput& in, model::Editor* editor, const Model& model, ViewState& view, unsigned in_port, unsigned in_channel) noexcept {
  if (r.take == nullptr) {
    return false;
  }
  r.in = &in;
  std::uint8_t track = LaneRec::kNone;
  seq::MidiRoute route{};
  if (r.step_open) {  // STEP ended (another mode, screen, track, pattern or instrument): the chord so far is merged where it began, the cursor stays
    std::uint8_t st = 0;
    seq::MidiRoute sr{};
    if (!step_wanted(model, view, st, sr) || st != r.step_track || view.edit.pattern != r.step_pattern || sr.ch != r.step_channel || sr.ports != r.step_port) {
      step_end(r, editor, model, view, false);
    }
  }
  const bool want = lane_wanted(model, view, track, route);
  if (r.capturing() && (!want || track != r.track || route.ch != r.channel || route.ports != r.port)) {
    stop(r, in, editor, model, view, in_port, in_channel);
    r.run = false;
  }
  if (!want) {
    if (lane_counting_in(model, view)) {
      r.run = true;  // armed: the count-in is part of the recording, its notice is said once through to the end of the take
    } else if (r.run) {
      r.run = false;
      r.cc_have = 0;
      r.said_cc = false;
      r.said_ci = false;
    }
    return false;
  }
  if (!r.capturing()) {
    if (r.retry) {  // the last take was refused for room and kept: one more try before a new one begins
      commit(r, editor, view, false);
      r.retry = false;
    }
    midi::StampMsg stale;
    for (unsigned n = 0; n < 2u * midi::CcInput::kStampRingSize && in.pop_stamped(stale); ++n) {
    }
    r.stamp_drops = in.stamp_dropped();
    r.take->reset();
    r.track = track;
    r.channel = route.ch;
    r.port = route.ports;
    r.pattern = 0;
    r.last_tick = 0;
    r.last_pos = 0;
    r.cc_have = 0;  // (a take's first move of a letter starts at CC 64, as the encoder's moves of the count-in were swallowed)
    if (!r.run) {
      r.said_cc = false;
      r.said_ci = false;
    }
    r.run = true;
    in.open_take(track);
  }
  drain(r, in, editor, model, view, in_port, in_channel, kLaneStampPerPass);
  if (view.play.count_in == 0u) {
    if (r.take->open() && track_pattern(view, track) != r.take->pattern()) {
      // The track went on to another pattern (the playhead is already in it; a Perform override counts, as the stamps' pattern does): the take of the old one ran to its end. Ended here, before the playhead's tick is taken as a position in the old pattern (a
      // false loop wrap would replace the lane's points of the whole pattern, and the held notes' LEN would be wrong).
      finish_take(r, editor, view, r.take->length_ticks() - 1u, false);
    } else {
      r.last_pos = playhead_tick(model, view, track);
    }
  }
  return true;
}

int lane_cc_letter(const LaneRec& r, const Model& model, const ViewState& view, core::ParamId id) noexcept {
  if ((!r.capturing() && !lane_counting_in(model, view)) || core::param_kind(id) != core::ParamKind::Instrument || static_cast<int>(core::param_index(id)) + 1 != static_cast<int>(view.instrument)) {
    return -1;
  }
  const unsigned first = model::instrument_param::midi_cc(0);
  const unsigned field = core::param_field(id);
  return field >= first && field < first + model::kMidiCcSlotsPerInstrument ? static_cast<int>(field - first) : -1;
}

bool lane_cc_move(LaneRec& r, model::Editor* editor, const Model& model, ViewState& view, unsigned letter, unsigned value) noexcept {
  static_assert(sizeof LaneRec::cc_now / sizeof LaneRec::cc_now[0] == model::kMidiCcSlotsPerInstrument, "one running value per CC letter");
  const unsigned idx = static_cast<unsigned>(view.instrument);
  if (r.take == nullptr || !r.capturing() || model.project == nullptr || idx < 1u || letter >= model::kMidiCcSlotsPerInstrument || view.play.count_in != 0u) {
    return false;  // (the count-in has no playhead on the track: the move would land on a stale tick)
  }
  const model::events::CcOut out = model::events::cc_resolve(model::events::env_of(*model.project), model.project->instrument(idx - 1u), static_cast<std::uint8_t>(letter));
  if (out.what != model::events::CcOut::What::Controller) {  // PROG, CH AT, POLY AT: not recorded in P1 (decision (c)); an unassigned letter sends nothing
    if (!r.said_cc) {
      r.said_cc = true;
      core::FixedString<core::kNotifyTextChars> text;
      text.append("LANE REC: CC ");
      const char l[2] = {static_cast<char>('A' + letter), '\0'};
      text.append(l);
      text.append(out.what == model::events::CcOut::What::Unassigned ? " IS UNASSIGNED" : " (PROG/AT) NOT RECORDED");
      say(view, core::Severity::Info, text.c_str());
    }
    return true;
  }
  midi::StampMsg m{};
  m.tick = static_cast<std::uint16_t>(r.last_pos);
  m.kind = midi::StampKind::Cc;
  m.port = r.port;
  m.channel = static_cast<std::uint8_t>(r.channel + 1u);
  m.a = out.number;
  m.b = static_cast<std::uint8_t>(value > 127u ? 127u : value);
  m.pattern = track_pattern(view, r.track);  // a Perform override counts, as for the keyboard's stamps
  feed(r, editor, model, view, m);  // the take's own thinning, split and notices, as for the keyboard's controller
  return true;
}

bool lane_step_on(LaneRec& r, model::Editor* editor, const Model& model, ViewState& view, std::uint8_t note, std::uint8_t velocity) noexcept {
  std::uint8_t track = 0;
  seq::MidiRoute route{};
  if (r.take == nullptr || r.capturing() || note > 127u || !step_wanted(model, view, track, route)) {
    return false;
  }
  if (!r.step_open) {
    const model::Pattern* p = model.project != nullptr ? model.project->pattern(view.edit.pattern) : nullptr;
    if (p == nullptr) {
      return false;
    }
    const std::uint32_t row = static_cast<std::uint32_t>(view.pattern.row());
    const std::uint32_t len = p->track_length[track];
    if (row >= len) {  // a track shorter than the pattern: the grid says the same
      say(view, core::Severity::Info, "PAST TRACK END");
      return true;
    }
    if (r.retry) {  // a take refused for room is still held in the buffer: its one more try comes before this chord uses it
      commit(r, editor, view, false);
      r.retry = false;
    }
    r.take->reset();
    r.step_tick = row * model::kLaneTicksPerStep;
    r.take->begin(view.edit.pattern, track, route.ch, route.ports, r.step_tick, len * model::kLaneTicksPerStep);
    r.step_open = true;
    r.step_n = 0;
    r.step_pattern = view.edit.pattern;
    r.step_track = track;
    r.step_channel = route.ch;
    r.step_port = route.ports;
    for (std::uint32_t& w : r.step_held) {
      w = 0;
    }
  }
  const std::uint32_t bit = 1u << (note & 31u);
  std::uint32_t& word = r.step_held[note >> 5];
  if ((word & bit) != 0u) {  // a repeat of a key already held (a controller's retrigger): no second note
    return true;
  }
  word |= bit;
  ++r.step_n;
  (void)r.take->note_len(r.step_tick, note, velocity != 0u ? velocity : 1u, model::kLaneTicksPerStep);
  return true;
}

bool lane_step_off(LaneRec& r, model::Editor* editor, const Model& model, ViewState& view, std::uint8_t note) noexcept {
  if (!r.step_open || note > 127u) {
    return false;
  }
  const std::uint32_t bit = 1u << (note & 31u);
  std::uint32_t& word = r.step_held[note >> 5];
  if ((word & bit) == 0u) {
    return false;
  }
  word &= ~bit;
  if (--r.step_n == 0u) {
    step_end(r, editor, model, view, true);
  }
  return true;
}

void lane_step_all_off(LaneRec& r, model::Editor* editor, const Model& model, ViewState& view) noexcept {
  step_end(r, editor, model, view, true);
}

void lane_discard(LaneRec& r) noexcept {
  if (r.in != nullptr) {
    r.in->close_take();
  }
  if (r.take != nullptr) {
    r.take->reset();
  }
  r.step_open = false;
  r.step_n = 0;
  r.track = LaneRec::kNone;
  r.retry = false;
  r.said_drop = false;
  r.cc_have = 0;
  r.said_cc = false;
  r.said_ci = false;
  r.run = false;
}

}  // namespace o3os::app
