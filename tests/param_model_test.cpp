// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-002d with a project: parameters of the instruments, tracks, master and the project's settings through the Project and the Editor
// (validation, a generation per parameter, undo and redo), and the SETS and LRNB chunks of the native format (round trip, default-fill,
// order, damage). One project in a static arena and a small journal, so this file also runs on the Cortex-M4F (qemu_params_model).
#include <doctest/doctest.h>

#include <o3os/formats/crc32.h>
#include <o3os/formats/native.h>
#include <o3os/model/editor.h>
#include <o3os/model/params.h>
#include <o3os/model/project.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "support/memory_io.h"
#include "support/need.h"
#include "support/rng.h"

using namespace o3os;
using core::ParamId;
using core::ParamKind;
namespace ip = model::instrument_param;
using Bytes = std::vector<std::uint8_t>;

// The target runs these loops about an order of magnitude slower than the host: fewer steps there, the same cases.
#if defined(__arm__)
constexpr unsigned kScale = 8;
#else
constexpr unsigned kScale = 1;
#endif

namespace {

model::ModelArena g_arena;  // zeroed, like the project region of SDRAM at boot
alignas(8) std::uint8_t g_ring[64 * 1024];

model::Project& project() { return g_arena.project; }

ParamId inst(unsigned slot, unsigned field) { return core::param_id(ParamKind::Instrument, slot, field); }
ParamId track(unsigned slot, unsigned field) { return core::param_id(ParamKind::Track, slot, field); }
ParamId master(unsigned field) { return core::param_id(ParamKind::Master, 0, field); }
ParamId setting(unsigned field) { return core::param_id(ParamKind::Project, 0, field); }

// A project of three sample instruments (slots 0, 1 and 2), the rest free.
void three_instruments() {
  project().reset();
  for (unsigned i = 0; i < 3; ++i) {
    model::Instrument in;
    model::init_instrument(in, model::InstrumentKind::Sample);
    in.name[0] = static_cast<char>('A' + i);
    CHECK(project().set_instrument(i, in) == model::Status::Ok);
  }
}

// Every parameter of the three instruments, two tracks, the master and the settings: 3 x 50 + 2 x 5 + 8 + 4 ids.
std::vector<ParamId> watched() {
  std::vector<ParamId> ids;
  for (unsigned i = 0; i < 3; ++i) {
    for (unsigned s = 0; s < ip::kCount; ++s) {
      ids.push_back(inst(i, model::param_field_of_slot(ParamKind::Instrument, s)));
    }
  }
  for (unsigned t = 0; t < 2; ++t) {
    for (unsigned f = 0; f < model::track_param::kCount; ++f) {
      ids.push_back(track(t, f));
    }
  }
  for (unsigned f = 0; f < model::master_param::kCount; ++f) {
    ids.push_back(master(f));
  }
  for (unsigned f = 0; f < model::project_setting::kCount; ++f) {
    ids.push_back(setting(f));
  }
  return ids;
}

std::vector<std::uint32_t> generations(const std::vector<ParamId>& ids) {
  std::vector<std::uint32_t> g;
  for (const ParamId id : ids) {
    g.push_back(project().param_generation(id));
  }
  return g;
}

// The ids whose generation differs between two snapshots.
std::set<std::uint32_t> moved(const std::vector<ParamId>& ids, const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b) {
  std::set<std::uint32_t> out;
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (a[i] != b[i]) {
      out.insert(static_cast<std::uint32_t>(ids[i]));
    }
  }
  return out;
}

std::set<std::uint32_t> ids_of(std::initializer_list<ParamId> l) {
  std::set<std::uint32_t> s;
  for (const ParamId id : l) {
    s.insert(static_cast<std::uint32_t>(id));
  }
  return s;
}

}  // namespace

// ---- the Project's parameters --------------------------------------------------------------------------------------------

TEST_CASE("project params: read and write each kind, refuse what is outside the range or does not exist") {
  three_instruments();
  model::Project& p = project();
  CHECK(p.check_invariants());
  // An instrument slot that is free has no parameters.
  CHECK_FALSE(p.has_param(inst(5, ip::Volume)));
  CHECK(p.get_param(inst(5, ip::Volume)) == 0.0f);
  CHECK(p.set_param(inst(5, ip::Volume), 10.0f) == model::Status::NotFound);
  // A used one: defaults, then edits.
  CHECK(p.has_param(inst(0, ip::Volume)));
  CHECK(p.get_param(inst(0, ip::Volume)) == 50.0f);  // our default
  CHECK(p.get_param(inst(0, ip::Cutoff)) == 1.0f);   // wide open, so that switching the filter on does not silence the instrument
  CHECK(p.set_param(inst(0, ip::Volume), 37.0f) == model::Status::Ok);
  CHECK(p.get_param(inst(0, ip::Volume)) == 37.0f);
  CHECK(p.instrument(0)->sampler.volume == 37);
  CHECK(p.set_param(inst(0, ip::Volume), 37.6f) == model::Status::Ok);  // an integer field rounds
  CHECK(p.get_param(inst(0, ip::Volume)) == 38.0f);
  CHECK(p.set_param(inst(0, ip::Cutoff), 0.375f) == model::Status::Ok);
  CHECK(p.instrument(0)->sampler.cutoff == 0.375f);
  CHECK(p.set_param(inst(1, ip::Tune), -7.0f) == model::Status::Ok);
  CHECK(p.instrument(1)->sampler.tune == -7);
  CHECK(p.set_param(inst(2, ip::envelope(5, ip::EnvRelease)), 4200.0f) == model::Status::Ok);
  CHECK(p.instrument(2)->sampler.envelope[5].release == 4200);
  CHECK(p.set_param(inst(2, ip::envelope(5, ip::EnvEnable)), 1.0f) == model::Status::Ok);
  CHECK(p.instrument(2)->sampler.envelope[5].enable == 1);
  CHECK(p.instrument(2)->sampler.envelope[4].enable == 0);
  // What is refused leaves everything as it was.
  const std::uint32_t generation = p.generation();
  CHECK(p.set_param(inst(0, ip::Volume), 101.0f) == model::Status::BadValue);
  CHECK(p.set_param(inst(0, ip::Volume), -1.0f) == model::Status::BadValue);
  CHECK(p.set_param(inst(0, ip::Volume), std::nanf("")) == model::Status::BadValue);
  CHECK(p.set_param(inst(0, ip::BitDepth), 3.0f) == model::Status::BadValue);
  CHECK(p.set_param(inst(0, ip::FilterType), 3.0f) == model::Status::BadValue);
  CHECK(p.set_param(inst(0, 14), 1.0f) == model::Status::BadIndex);
  CHECK(p.set_param(ParamId::None, 1.0f) == model::Status::BadIndex);
  CHECK(p.set_param(core::param_id(ParamKind::Device, 0, 0), 1.0f) == model::Status::BadIndex);  // the device's, not a project's
  CHECK(p.set_param(core::param_id(ParamKind::Instrument, 64, 0), 1.0f) == model::Status::BadIndex);
  CHECK(p.get_param(core::param_id(ParamKind::Device, 0, 0)) == 0.0f);
  CHECK(p.generation() == generation);
  // Tracks, master and settings.
  CHECK(p.get_param(track(3, model::track_param::Volume)) == 86.0f);
  CHECK(p.set_param(track(3, model::track_param::Volume), 20.0f) == model::Status::Ok);
  CHECK(p.track(3)->volume == 20);
  CHECK(p.set_param(track(3, model::track_param::Mute), 1.0f) == model::Status::Ok);
  CHECK(p.set_param(track(3, model::track_param::Solo), 1.0f) == model::Status::Ok);
  CHECK(p.track(3)->flags == (model::kTrackMute | model::kTrackSolo));
  CHECK(p.set_param(track(3, model::track_param::Mute), 0.0f) == model::Status::Ok);
  CHECK(p.track(3)->flags == model::kTrackSolo);
  CHECK(p.set_param(track(15, model::track_param::Bus), 16.0f) == model::Status::Ok);
  CHECK(p.set_param(track(15, model::track_param::Bus), 17.0f) == model::Status::BadValue);
  CHECK(p.set_param(master(model::master_param::Limiter), 1.0f) == model::Status::Ok);
  CHECK(p.master().flags == model::kMasterLimiter);
  CHECK(p.set_param(master(model::master_param::ReverbMute), 1.0f) == model::Status::Ok);
  CHECK(p.master().flags == (model::kMasterLimiter | model::kMasterReverbMute));
  CHECK(p.set_param(master(model::master_param::Delay), 12.0f) == model::Status::Ok);
  CHECK(p.master().delay_volume == 12);
  CHECK(p.get_param(setting(model::project_setting::AntiAliasing)) == 1.0f);
  CHECK(p.set_param(setting(model::project_setting::SendsMode), 1.0f) == model::Status::Ok);
  CHECK(p.settings().get(model::project_setting::SendsMode) == 1.0f);
  CHECK(p.set_param(setting(model::project_setting::SendsMode), 2.0f) == model::Status::BadValue);
  CHECK(p.set_param(setting(4), 1.0f) == model::Status::BadIndex);
  CHECK(p.check_invariants());
}

TEST_CASE("project params: a generation per parameter moves for that parameter alone") {
  three_instruments();
  model::Project& p = project();
  const std::vector<ParamId> ids = watched();
  NEED(ids.size() == 3u * 95u + 2u * 5u + 8u + 3u);
  auto before = generations(ids);
  const std::uint32_t whole = p.generation();
  const std::uint32_t instrument1 = p.instrument_generation(1);
  const std::uint32_t instrument0 = p.instrument_generation(0);
  const std::uint32_t tracks = p.tracks_generation();

  // One edit: that parameter, its instrument's and the project's, and nothing else.
  CHECK(p.set_param(inst(1, ip::Cutoff), 0.5f) == model::Status::Ok);
  auto after = generations(ids);
  CHECK(moved(ids, before, after) == ids_of({inst(1, ip::Cutoff)}));
  CHECK(p.param_generation(inst(1, ip::Cutoff)) == before[50 + ip::Cutoff] + 1u);
  CHECK(p.instrument_generation(1) == instrument1 + 1u);
  CHECK(p.instrument_generation(0) == instrument0);
  CHECK(p.generation() == whole + 1u);
  CHECK(p.tracks_generation() == tracks);

  // The same value, a refused value, a free slot, a missing parameter: no change anywhere.
  before = after;
  CHECK(p.set_param(inst(1, ip::Cutoff), 0.5f) == model::Status::Ok);
  CHECK(p.set_param(inst(1, ip::Cutoff), 7.0f) == model::Status::BadValue);
  CHECK(p.set_param(inst(9, ip::Cutoff), 0.5f) == model::Status::NotFound);
  CHECK(p.set_param(inst(1, ip::mod(6, 0)), 0.5f) == model::Status::BadIndex);
  CHECK(generations(ids) == before);
  CHECK(p.generation() == whole + 1u);

  // A track, the master and a setting are counted too, and a track or master edit is the "tracks" generation.
  CHECK(p.set_param(track(1, model::track_param::Pan), 10.0f) == model::Status::Ok);
  CHECK(p.tracks_generation() == tracks + 1u);
  after = generations(ids);
  CHECK(moved(ids, before, after) == ids_of({track(1, model::track_param::Pan)}));
  before = after;
  CHECK(p.set_param(master(model::master_param::DryMute), 1.0f) == model::Status::Ok);
  CHECK(p.set_param(setting(model::project_setting::AntiAliasing), 0.0f) == model::Status::Ok);
  after = generations(ids);
  CHECK(moved(ids, before, after) == ids_of({master(model::master_param::DryMute), setting(model::project_setting::AntiAliasing)}));
  before = after;

  // A whole instrument written: only the parameters whose value differs advance (the name is not a parameter).
  model::Instrument in = *p.instrument(1);
  in.sampler.tune = 5;
  in.sampler.envelope[3].decay = 700;
  in.sampler.resonance = 0.25f;
  in.name[0] = 'Z';
  CHECK(p.set_instrument(1, in) == model::Status::Ok);
  after = generations(ids);
  CHECK(moved(ids, before, after) ==
        ids_of({inst(1, ip::Tune), inst(1, ip::envelope(3, ip::EnvDecay)), inst(1, ip::Resonance)}));
  before = after;
  CHECK(p.set_instrument(1, in) == model::Status::Ok);  // the same again: no parameter moves
  CHECK(generations(ids) == before);
  // A free slot taken: all 50 of it are new; a slot cleared: all 50 are gone.
  model::Instrument fresh;
  model::init_instrument(fresh, model::InstrumentKind::Sample);
  CHECK(p.set_instrument(3, fresh) == model::Status::Ok);
  CHECK(p.param_generation(inst(3, ip::Volume)) != 0u);
  std::vector<std::uint32_t> slot2_before;
  for (unsigned s = 0; s < ip::kCount; ++s) {
    slot2_before.push_back(p.param_generation(inst(2, model::param_field_of_slot(ParamKind::Instrument, s))));
  }
  CHECK(p.clear_instrument(2) == model::Status::Ok);
  after = generations(ids);
  CHECK(moved(ids, before, after).size() == 95u);
  for (unsigned s = 0; s < ip::kCount; ++s) {
    CHECK(p.param_generation(inst(2, model::param_field_of_slot(ParamKind::Instrument, s))) == slot2_before[s] + 1u);
  }
  before = after;
  // A whole track and the whole master written.
  model::TrackSlot t = *p.track(1);
  t.volume = 33;
  t.flags = model::kTrackMute;
  CHECK(p.set_track(1, t) == model::Status::Ok);
  after = generations(ids);
  CHECK(moved(ids, before, after) == ids_of({track(1, model::track_param::Volume), track(1, model::track_param::Mute)}));  // flags 0 -> mute: solo stays off
  before = after;
  model::MasterSettings m = p.master();
  m.reverb_volume = 5;
  m.flags = static_cast<std::uint8_t>(m.flags ^ model::kMasterLimiter);
  CHECK(p.set_master(m) == model::Status::Ok);
  after = generations(ids);
  CHECK(moved(ids, before, after) == ids_of({master(model::master_param::Reverb), master(model::master_param::Limiter)}));
  before = after;
  // The settings block assigned whole.
  model::Settings block = p.settings();
  CHECK(block.set(model::project_setting::LimiterMode, 1.0f) == model::Status::Ok);
  CHECK(p.set_settings(block) == model::Status::Ok);
  after = generations(ids);
  CHECK(moved(ids, before, after) == ids_of({setting(model::project_setting::LimiterMode)}));
  before = after;
  CHECK(p.set_settings(block) == model::Status::Ok);  // the same again: nothing moves, the project does not change
  const std::uint32_t g = p.generation();
  CHECK(p.set_settings(block) == model::Status::Ok);
  CHECK(p.generation() == g);
  CHECK(generations(ids) == before);
  // reset(): every parameter's generation moves.
  p.reset();
  after = generations(ids);
  CHECK(moved(ids, before, after).size() == ids.size());
  CHECK(p.check_invariants());
}

TEST_CASE("project params: a generation never repeats a value it had, whatever the edits") {
  three_instruments();
  model::Project& p = project();
  const std::vector<ParamId> ids = watched();
  test::Rng rng(20261011u);
  std::map<std::uint32_t, std::set<std::uint32_t>> seen;  // per parameter, the generations it has shown
  std::vector<std::uint32_t> last = generations(ids);
  for (std::size_t i = 0; i < ids.size(); ++i) {
    seen[static_cast<std::uint32_t>(ids[i])].insert(last[i]);
  }
  unsigned changes = 0;
  for (unsigned step = 0; step < 6000 / kScale; ++step) {
    const std::uint32_t what = rng.below(100);
    if (what < 70) {
      const ParamId id = ids[rng.below(static_cast<std::uint32_t>(ids.size()))];
      const core::ParamDesc* d = model::param_desc(id);
      NEED(d != nullptr);
      (void)p.set_param(id, core::param_denorm(*d, static_cast<float>(rng.below(5)) * 0.25f));  // few values: often the same one again
    } else if (what < 80) {
      model::Instrument in;
      model::init_instrument(in, model::InstrumentKind::Sample);
      in.sampler.volume = static_cast<std::uint8_t>(rng.below(3) * 50);
      in.sampler.envelope[rng.below(6)].attack = static_cast<std::uint16_t>(rng.below(2) * 100);
      (void)p.set_instrument(rng.below(3), in);
    } else if (what < 85) {
      (void)p.clear_instrument(rng.below(3));
      model::Instrument in;
      model::init_instrument(in, model::InstrumentKind::Sample);
      (void)p.set_instrument(rng.below(3), in);
    } else if (what < 92) {
      model::TrackSlot t = *p.track(rng.below(2));
      t.volume = static_cast<std::uint8_t>(rng.below(3) * 40);
      t.flags = static_cast<std::uint8_t>(rng.below(4));
      (void)p.set_track(rng.below(2), t);
    } else if (what < 97) {
      model::MasterSettings m = p.master();
      m.flags = static_cast<std::uint8_t>(rng.below(16));
      (void)p.set_master(m);
    } else if (what < 99) {
      model::Settings s = p.settings();
      (void)s.set(rng.below(4), static_cast<float>(rng.below(2)));
      (void)p.set_settings(s);
    } else {
      p.reset();
      for (unsigned i = 0; i < 3; ++i) {
        model::Instrument in;
        model::init_instrument(in, model::InstrumentKind::Sample);
        (void)p.set_instrument(i, in);
      }
    }
    const std::vector<std::uint32_t> now = generations(ids);
    for (std::size_t i = 0; i < ids.size(); ++i) {
      std::set<std::uint32_t>& history = seen[static_cast<std::uint32_t>(ids[i])];
      if (now[i] != last[i]) {
        ++changes;
        CHECK_MESSAGE(history.insert(now[i]).second, "parameter " << static_cast<std::uint32_t>(ids[i]) << " repeated a generation at step " << step);
      }
    }
    last = now;
  }
  CHECK(changes > 3000u / kScale);
  CHECK(p.check_invariants());
}

TEST_CASE("project params: a fresh project's tracks, master and settings are the descriptors' defaults") {
  project().reset();
  model::Project& p = project();
  for (unsigned t : {0u, 7u, 15u}) {
    for (unsigned f = 0; f < model::track_param::kCount; ++f) {
      const core::ParamDesc* d = model::param_desc(track(t, f));
      NEED(d != nullptr);
      CHECK_MESSAGE(p.get_param(track(t, f)) == d->def, d->name);
    }
  }
  for (unsigned f = 0; f < model::master_param::kCount; ++f) {
    CHECK(p.get_param(master(f)) == model::param_desc(master(f))->def);
  }
  for (unsigned f = 0; f < model::project_setting::kCount; ++f) {
    CHECK(p.get_param(setting(f)) == model::param_desc(setting(f))->def);
  }
  CHECK(p.settings().is_default());
  CHECK(p.settings().kind() == ParamKind::Project);
}

// ---- the Editor: parameters are undoable -------------------------------------------------------------------------------------

TEST_CASE("editor params: one edit is one unit of 40 bytes, and undo and redo bring the value back exactly") {
  three_instruments();
  model::UndoJournal journal;
  CHECK(journal.begin(g_ring, sizeof g_ring));
  model::Editor e(project(), journal);
  model::Project& p = project();
  CHECK(e.set_param(inst(0, ip::Cutoff), 0.8f) == model::Status::Ok);
  CHECK(journal.used_bytes() == model::UndoJournal::unit_bytes_for(12));
  CHECK(model::UndoJournal::unit_bytes_for(12) == 40u);
  CHECK(journal.depth() == 1u);
  CHECK(p.get_param(inst(0, ip::Cutoff)) == 0.8f);
  const std::uint32_t g = p.param_generation(inst(0, ip::Cutoff));

  CHECK(e.undo() == model::UndoResult::Ok);
  CHECK(p.get_param(inst(0, ip::Cutoff)) == 1.0f);
  CHECK(p.param_generation(inst(0, ip::Cutoff)) == g + 1u);  // undo moves the generation like an edit
  NEED(e.applied_count() == 1u);
  CHECK(e.applied(0).id == inst(0, ip::Cutoff));
  CHECK(e.applied(0).value == 1.0f);
  CHECK_FALSE(e.applied_incomplete());
  CHECK(e.redo() == model::UndoResult::Ok);
  CHECK(p.get_param(inst(0, ip::Cutoff)) == 0.8f);
  NEED(e.applied_count() == 1u);
  CHECK(e.applied(0).value == 0.8f);

  // The value that is journalled is the one that was stored: an integer field rounds, and redo applies that.
  CHECK(e.set_param(inst(0, ip::Volume), 37.6f) == model::Status::Ok);
  CHECK(e.undo() == model::UndoResult::Ok);
  CHECK(p.get_param(inst(0, ip::Volume)) == 50.0f);
  CHECK(e.redo() == model::UndoResult::Ok);
  CHECK(p.get_param(inst(0, ip::Volume)) == 38.0f);

  // Nothing is recorded for a refused edit or one that changes nothing, and neither ends the redo history.
  CHECK(e.undo() == model::UndoResult::Ok);
  const unsigned depth = journal.depth();
  CHECK(e.set_param(inst(0, ip::Volume), 50.0f) == model::Status::Ok);  // equal to what is there now
  CHECK(e.set_param(inst(0, ip::Volume), 101.0f) == model::Status::BadValue);
  CHECK(e.set_param(inst(7, ip::Volume), 5.0f) == model::Status::NotFound);
  CHECK(journal.depth() == depth);
  CHECK(journal.redo_depth() == 1u);
  // A new edit does end it.
  CHECK(e.set_param(inst(0, ip::Panning), 10.0f) == model::Status::Ok);
  CHECK(journal.redo_depth() == 0u);
  CHECK(e.redo() == model::UndoResult::Nothing);
  CHECK(e.applied_count() == 0u);  // cleared by every undo and redo
}

TEST_CASE("editor params: a group of edits is one unit, undone together, last edit first") {
  three_instruments();
  model::UndoJournal journal;
  CHECK(journal.begin(g_ring, sizeof g_ring));
  model::Editor e(project(), journal);
  model::Project& p = project();
  e.begin_group(core::action_id("param.edit"), 5);
  CHECK(e.set_param(inst(1, ip::Volume), 10.0f) == model::Status::Ok);
  CHECK(e.set_param(inst(1, ip::Panning), 20.0f) == model::Status::Ok);
  CHECK(e.set_param(track(2, model::track_param::Mute), 1.0f) == model::Status::Ok);
  e.end_group();
  CHECK(journal.depth() == 1u);
  core::ActionId a = core::ActionId::None;
  core::ActionArg arg = 0;
  CHECK(journal.newest_undo(a, arg));
  CHECK((a == core::action_id("param.edit") && arg == 5));
  CHECK(e.undo() == model::UndoResult::Ok);
  CHECK(p.get_param(inst(1, ip::Volume)) == 50.0f);
  CHECK(p.get_param(inst(1, ip::Panning)) == 50.0f);
  CHECK(p.get_param(track(2, model::track_param::Mute)) == 0.0f);
  NEED(e.applied_count() == 3u);
  CHECK(e.applied(0).id == track(2, model::track_param::Mute));  // the way back: the last edit is undone first
  CHECK(e.applied(1).id == inst(1, ip::Panning));
  CHECK(e.applied(2).id == inst(1, ip::Volume));
  CHECK(e.redo() == model::UndoResult::Ok);
  CHECK(p.get_param(inst(1, ip::Volume)) == 10.0f);
  CHECK(e.applied(0).id == inst(1, ip::Volume));  // and forwards in the order they were made
  CHECK(e.applied(2).id == track(2, model::track_param::Mute));
  // More than the list holds: the first kAppliedMax are listed and the list says it is short.
  e.begin_group(core::action_id("macro.play"), 0);
  for (unsigned i = 0; i < 20; ++i) {
    CHECK(e.set_param(inst(0, ip::envelope(i % 6, ip::EnvAttack)), static_cast<float>(100 + i)) == model::Status::Ok);
  }
  e.end_group();
  CHECK(e.undo() == model::UndoResult::Ok);
  CHECK(e.applied_count() == model::Editor::kAppliedMax);
  CHECK(e.applied_incomplete());
}

TEST_CASE("editor params: a whole instrument, track or master replaced is not in the applied list, which says so") {
  three_instruments();
  model::UndoJournal journal;
  CHECK(journal.begin(g_ring, sizeof g_ring));
  model::Editor e(project(), journal);
  model::Instrument in = *project().instrument(0);
  in.sampler.tune = 4;
  CHECK(e.set_instrument(0, in) == model::Status::Ok);
  CHECK(e.undo() == model::UndoResult::Ok);
  CHECK(e.applied_count() == 0u);
  CHECK(e.applied_incomplete());
  CHECK(project().get_param(inst(0, ip::Tune)) == 0.0f);
  CHECK(e.redo() == model::UndoResult::Ok);
  CHECK(e.applied_incomplete());
  model::TrackSlot t = *project().track(0);
  t.pan = 80;
  CHECK(e.set_track(0, t) == model::Status::Ok);
  CHECK(e.undo() == model::UndoResult::Ok);
  CHECK(e.applied_incomplete());
  CHECK(e.set_param(inst(0, ip::Tune), 9.0f) == model::Status::Ok);
  CHECK(e.undo() == model::UndoResult::Ok);
  CHECK_FALSE(e.applied_incomplete());  // a plain parameter edit is complete again
  // A project changed behind the editor's back is noticed, as for every other edit.
  CHECK(project().set_param(inst(0, ip::Tune), 3.0f) == model::Status::Ok);
  CHECK(e.undo() == model::UndoResult::Stale);
}

TEST_CASE("editor params: Learn bindings are edits too: one slot, new and old, undone exactly") {
  three_instruments();
  model::UndoJournal journal;
  CHECK(journal.begin(g_ring, sizeof g_ring));
  model::Editor e(project(), journal);
  model::Project& p = project();
  const core::LearnBinding a{inst(0, ip::Cutoff), 74, 0, 0, 0};
  const core::LearnBinding b{inst(0, ip::Resonance), 71, 3, 1, 0};
  CHECK(e.set_learn_slot(0, a) == model::Status::Ok);
  CHECK(journal.used_bytes() == model::UndoJournal::unit_bytes_for(17));
  CHECK(model::UndoJournal::unit_bytes_for(17) == 48u);
  CHECK(e.set_learn_slot(1, b) == model::Status::Ok);
  CHECK(e.set_learn_slot(1, b) == model::Status::Ok);  // the same again: no unit
  CHECK(journal.depth() == 2u);
  CHECK(p.settings().learn_count() == 2u);
  // Refused: a binding of the device's scope, a bad controller, a slot past the store.
  CHECK(e.set_learn_slot(2, core::LearnBinding{track(1, 0), 5, 0, 0, 0}) == model::Status::BadValue);
  CHECK(e.set_learn_slot(2, core::LearnBinding{inst(0, ip::Volume), 200, 0, 0, 0}) == model::Status::BadValue);
  CHECK(e.set_learn_slot(core::kLearnSlots, a) == model::Status::BadIndex);
  CHECK(journal.depth() == 2u);
  // Replacing one and emptying one, in a group: one undo takes both back.
  e.begin_group(core::action_id("learn.mode"), 0);
  CHECK(e.set_learn_slot(0, core::LearnBinding{inst(0, ip::Cutoff), 20, 1, 2, 0}) == model::Status::Ok);
  CHECK(e.set_learn_slot(1, core::LearnBinding{}) == model::Status::Ok);
  e.end_group();
  CHECK(p.settings().learn_count() == 1u);
  CHECK(e.undo() == model::UndoResult::Ok);
  CHECK(std::memcmp(&p.settings().learn(0), &a, sizeof a) == 0);
  CHECK(std::memcmp(&p.settings().learn(1), &b, sizeof b) == 0);
  CHECK(e.redo() == model::UndoResult::Ok);
  CHECK(p.settings().learn(0).cc == 20);
  CHECK(p.settings().learn(1).param == ParamId::None);
  CHECK(e.undo() == model::UndoResult::Ok);
  CHECK(e.undo() == model::UndoResult::Ok);
  CHECK(e.undo() == model::UndoResult::Ok);
  CHECK(p.settings().learn_count() == 0u);
  CHECK(e.undo() == model::UndoResult::Nothing);
  CHECK(p.check_invariants());
}

namespace {

// What the property test watches: twelve parameters over the kinds, and the whole Learn store.
struct Snapshot {
  float value[12];
  core::LearnBinding learn[core::kLearnSlots];
};

const ParamId kWatch[12] = {inst(0, ip::Volume),       inst(0, ip::Cutoff),       inst(1, ip::Tune),
                            inst(1, ip::envelope(4, ip::EnvAmount)), inst(2, ip::BitDepth), inst(2, ip::FilterType),
                            track(0, model::track_param::Pan),       track(1, model::track_param::Mute), track(1, model::track_param::Solo),
                            master(model::master_param::Volume),     master(model::master_param::Limiter), setting(model::project_setting::SendsMode)};

Snapshot take() {
  Snapshot s;
  for (unsigned i = 0; i < 12; ++i) {
    s.value[i] = project().get_param(kWatch[i]);
  }
  for (unsigned i = 0; i < core::kLearnSlots; ++i) {
    s.learn[i] = project().settings().learn(i);
  }
  return s;
}

bool equal(const Snapshot& a, const Snapshot& b) { return std::memcmp(&a, &b, sizeof a) == 0; }

}  // namespace

TEST_CASE("editor params: random edits, binds, undos and redos against full snapshots") {
  static_assert(sizeof(Snapshot) == 12 * 4 + 32 * 8, "no padding: compared as bytes");
  three_instruments();
  model::UndoJournal journal;
  CHECK(journal.begin(g_ring, sizeof g_ring));
  model::Editor e(project(), journal);
  test::Rng rng(31415926u);
  std::vector<Snapshot> undo_stack, redo_stack;
  Snapshot now = take();
  unsigned edits = 0, undos = 0, redos = 0;
  for (unsigned step = 0; step < 4000 / kScale; ++step) {
    const unsigned what = rng.below(100);
    if (what < 45) {  // a parameter, sometimes out of range (refused)
      const unsigned w = rng.below(12);
      const core::ParamDesc* d = model::param_desc(kWatch[w]);
      NEED(d != nullptr);
      const float v = rng.below(10) == 0 ? d->max + 3.0f : core::param_denorm(*d, static_cast<float>(rng.below(5)) * 0.25f);
      const model::Status st = e.set_param(kWatch[w], v);
      const Snapshot after = take();
      CHECK((st == model::Status::Ok) == (v <= d->max));
      if (!equal(now, after)) {
        undo_stack.push_back(now);
        redo_stack.clear();
        ++edits;
      }
      now = after;
    } else if (what < 60) {  // a binding by the rules of the store, in one group: one unit (or none when it changes nothing)
      const ParamId target = kWatch[rng.below(8)];
      const core::LearnBinding b{target, static_cast<std::uint8_t>(rng.below(6)), static_cast<std::uint8_t>(rng.below(3)),
                                 static_cast<std::uint8_t>(rng.below(3)), 0};
      core::LearnPlan plan{0, 0};
      if (core::learn_scope(core::param_kind(target)) != core::LearnScope::Project) {
        CHECK(e.set_learn_slot(0, b) == model::Status::BadValue);  // a mixer parameter: the device's store
        continue;
      }
      if (!core::learn_plan(project().settings().learn_slots(), core::kLearnSlots, b, plan)) {
        continue;
      }
      e.begin_group(core::action_id("midi.cc"), 0);
      for (unsigned s = 0; s < core::kLearnSlots; ++s) {
        if ((plan.clear & (1u << s)) != 0) {
          CHECK(e.set_learn_slot(s, core::LearnBinding{}) == model::Status::Ok);
        }
      }
      CHECK(e.set_learn_slot(static_cast<unsigned>(plan.write), b) == model::Status::Ok);
      e.end_group();
      CHECK(project().settings().check_invariants());
      const Snapshot after = take();
      if (!equal(now, after)) {
        undo_stack.push_back(now);
        redo_stack.clear();
        ++edits;
      }
      now = after;
    } else if (what < 80) {
      const model::UndoResult r = e.undo();
      if (undo_stack.empty()) {
        CHECK(r == model::UndoResult::Nothing);
      } else {
        CHECK(r == model::UndoResult::Ok);
        redo_stack.push_back(now);
        now = undo_stack.back();
        undo_stack.pop_back();
        ++undos;
      }
      CHECK(equal(take(), now));
    } else {
      const model::UndoResult r = e.redo();
      if (redo_stack.empty()) {
        CHECK(r == model::UndoResult::Nothing);
      } else {
        CHECK(r == model::UndoResult::Ok);
        undo_stack.push_back(now);
        now = redo_stack.back();
        redo_stack.pop_back();
        ++redos;
      }
      CHECK(equal(take(), now));
    }
    CHECK(journal.depth() == undo_stack.size());
    CHECK(journal.redo_depth() == redo_stack.size());
  }
  CHECK(edits > 500u / kScale);
  CHECK(undos > 300u / kScale);
  CHECK(redos > 100u / kScale);
  CHECK(project().check_invariants());
  // All the way back to where the session began, and all the way forward again: to the end of what was redoable.
  const Snapshot first = undo_stack.empty() ? now : undo_stack.front();
  const Snapshot last = redo_stack.empty() ? now : redo_stack.front();
  while (e.undo() == model::UndoResult::Ok) {
  }
  CHECK(equal(take(), first));
  while (e.redo() == model::UndoResult::Ok) {
  }
  CHECK(equal(take(), last));
}

TEST_CASE("editor params: a ring that holds a dozen units drops the oldest whole and undoes the rest exactly") {
  three_instruments();
  alignas(8) static std::uint8_t small[512];
  model::UndoJournal journal;
  CHECK(journal.begin(small, sizeof small));
  model::Editor e(project(), journal);
  std::vector<float> history = {project().get_param(inst(0, ip::Volume))};
  for (unsigned i = 1; i <= 40; ++i) {
    CHECK(e.set_param(inst(0, ip::Volume), static_cast<float>(i)) == model::Status::Ok);
    history.push_back(static_cast<float>(i));
  }
  CHECK(journal.dropped_units() > 20u);
  const unsigned depth = journal.depth();
  CHECK(depth >= 8u);
  CHECK(depth <= 512u / 40u);
  for (unsigned back = 1; back <= depth; ++back) {
    CHECK(e.undo() == model::UndoResult::Ok);
    CHECK(project().get_param(inst(0, ip::Volume)) == history[history.size() - 1 - back]);
  }
  CHECK(e.undo() == model::UndoResult::Nothing);
}

// ---- the project file: SETS and LRNB -----------------------------------------------------------------------------------------

namespace {

constexpr std::uint32_t kSets = formats::native::kChunkSets;
constexpr std::uint32_t kLrnb = formats::native::kChunkLrnb;

Bytes save() {
  test::VectorSink sink;
  formats::native::Writer w;
  w.begin(project(), sink);
  while (w.step(4096) == formats::native::Progress::Running) {
  }
  CHECK(w.progress() == formats::native::Progress::Done);
  CHECK(w.report().error == formats::native::Error::None);
  CHECK(w.sized());
  CHECK(w.total_bytes() == sink.bytes.size());  // the sizing pre-pass counted the settings chunks too
  return sink.bytes;
}

formats::native::Report load(const Bytes& bytes, std::size_t budget = 4096) {
  static formats::native::Reader r;
  test::MemorySource src(bytes);
  r.begin(src, g_arena);
  while (r.step(budget) == formats::native::Progress::Running) {
  }
  return r.report();
}

struct Chunk {
  std::uint32_t id;
  Bytes payload;
};

std::uint32_t get32(const Bytes& b, std::size_t at) {
  return (static_cast<std::uint32_t>(b[at]) << 24) | (static_cast<std::uint32_t>(b[at + 1]) << 16) | (static_cast<std::uint32_t>(b[at + 2]) << 8) | b[at + 3];
}

std::vector<Chunk> chunks_of(const Bytes& file) {
  std::vector<Chunk> out;
  std::size_t pos = 12;
  while (pos + 8 <= file.size()) {
    const std::uint32_t id = get32(file, pos);
    const std::uint32_t len = get32(file, pos + 4);
    pos += 8;
    if (id == formats::native::kChunkCrc) {
      break;
    }
    out.push_back(Chunk{id, Bytes(file.begin() + static_cast<std::ptrdiff_t>(pos), file.begin() + static_cast<std::ptrdiff_t>(pos + len))});
    pos += len + (len & 1u);
  }
  return out;
}

void put32(Bytes& b, std::uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) {
    b.push_back(static_cast<std::uint8_t>(v >> s));
  }
}

Bytes rebuild(const std::vector<Chunk>& chunks) {
  Bytes body;
  for (const Chunk& c : chunks) {
    put32(body, c.id);
    put32(body, static_cast<std::uint32_t>(c.payload.size()));
    body.insert(body.end(), c.payload.begin(), c.payload.end());
    if ((c.payload.size() & 1u) != 0) {
      body.push_back(0);
    }
  }
  Bytes f = {'F', 'O', 'R', 'M'};
  put32(f, static_cast<std::uint32_t>(4 + body.size() + 14));
  f.insert(f.end(), {'O', '3', 'P', 'J'});
  f.insert(f.end(), body.begin(), body.end());
  const std::uint32_t crc = formats::crc32(f.data(), f.size());
  put32(f, formats::native::kChunkCrc);
  put32(f, 6);
  f.push_back(1);
  f.push_back(0);
  put32(f, crc);
  return f;
}

bool has(const std::vector<Chunk>& cs, std::uint32_t id) {
  for (const Chunk& c : cs) {
    if (c.id == id) {
      return true;
    }
  }
  return false;
}

Chunk& find(std::vector<Chunk>& cs, std::uint32_t id) {
  for (Chunk& c : cs) {
    if (c.id == id) {
      return c;
    }
  }
  static Chunk none;
  return none;
}

void u32_at(Bytes& b, std::size_t at, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    b[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (24 - 8 * i));
  }
}

void setup_with_settings() {
  three_instruments();
  model::Project& p = project();
  CHECK(p.set_param(setting(model::project_setting::SendsMode), 1.0f) == model::Status::Ok);
  CHECK(p.set_param(setting(model::project_setting::LimiterMode), 1.0f) == model::Status::Ok);
  CHECK(p.set_learn_slot(0, core::LearnBinding{inst(0, ip::Cutoff), 74, 0, 0, 0}) == model::Status::Ok);
  CHECK(p.set_learn_slot(1, core::LearnBinding{inst(0, ip::Resonance), 71, 1, 1, 0}) == model::Status::Ok);
  CHECK(p.set_learn_slot(2, core::LearnBinding{inst(0, ip::Volume), 7, 0, 2, core::kLearnFollow}) == model::Status::Ok);
}

}  // namespace

TEST_CASE("native settings: a project of default settings is the file it always was, with no SETS and no LRNB") {
  project().reset();
  const Bytes a = save();
  CHECK(a.size() == 186u);  // docs/formats/o3-project.md section 4
  const std::vector<Chunk> cs = chunks_of(a);
  CHECK_FALSE(has(cs, kSets));
  CHECK_FALSE(has(cs, kLrnb));
  CHECK(model::is_printable_fourcc(kSets));  // so a reader from before this slice keeps them as unknown chunks
  CHECK(model::is_printable_fourcc(kLrnb));
  // Bindings alone, settings alone: only the chunk that has something to say.
  CHECK(project().set_learn_slot(0, core::LearnBinding{inst(0, ip::Cutoff), 74, 0, 0, 0}) == model::Status::Ok);
  std::vector<Chunk> only_learn = chunks_of(save());
  CHECK_FALSE(has(only_learn, kSets));
  CHECK(has(only_learn, kLrnb));
  project().reset();
  CHECK(project().set_param(setting(model::project_setting::LimiterMode), 1.0f) == model::Status::Ok);
  std::vector<Chunk> only_sets = chunks_of(save());
  CHECK(has(only_sets, kSets));
  CHECK_FALSE(has(only_sets, kLrnb));
  NEED(find(only_sets, kSets).payload.size() == 4u + 3u * 8u);  // every setting, once one is not its default
}

TEST_CASE("native settings: values and bindings survive a save and a load, and the second save is the first") {
  setup_with_settings();
  const Bytes a = save();
  const std::vector<Chunk> cs = chunks_of(a);
  CHECK(has(cs, kSets));
  CHECK(has(cs, kLrnb));
  // After the instruments, before the CRC: the canonical order of docs/formats/o3-project.md section 2.
  NEED(cs.size() >= 7u);
  CHECK(cs[cs.size() - 2].id == kSets);
  CHECK(cs[cs.size() - 1].id == kLrnb);
  CHECK(cs[cs.size() - 3].id == formats::native::kChunkInst);

  const Snapshot before = take();
  const model::Settings settings_before = project().settings();
  project().reset();  // a different project in the arena
  CHECK(project().settings().is_default());
  const formats::native::Report r = load(a);
  CHECK(r.error == formats::native::Error::None);
  CHECK(r.settings_skipped == 0u);
  CHECK(equal(take(), before));
  CHECK(project().settings().get(model::project_setting::SendsMode) == 1.0f);
  CHECK(project().settings().get(model::project_setting::LimiterMode) == 1.0f);
  CHECK(project().settings().get(model::project_setting::AntiAliasing) == 1.0f);  // not in the file's differences: still the default
  CHECK(std::memcmp(project().settings().learn_slots(), settings_before.learn_slots(), core::kLearnSlots * sizeof(core::LearnBinding)) == 0);
  CHECK(project().check_invariants());
  CHECK(save() == a);
  // Whatever the step budget, and a source that stalls and comes back.
  for (const std::size_t budget : {std::size_t{1}, std::size_t{7}, std::size_t{4096}}) {
    CHECK(load(a, budget).error == formats::native::Error::None);
    CHECK(save() == a);
  }
}

// OS-INSFX S7 (docs/specs/OS-INSFX.md): the ids of an insert's parameters at the model level. The master insert (S6) is where 16, slot 0 of the project's INSX record and index 32 of the id;
// nothing here needs the audio side.
TEST_CASE("insert params: the id maps to the INSX record (a track's slot, the master's index 32), reads the blob, and its generation moves with the value") {
  three_instruments();
  model::Project& p = project();
  CHECK(p.add_module("fx", 2, 1, 0) == model::Status::Ok);
  const auto ins = [](unsigned where, unsigned slot, unsigned field) { return core::param_id(ParamKind::Insert, core::insert_param_index(where, slot), field); };
  CHECK(core::insert_param_index(0, 0) == 0u);
  CHECK(core::insert_param_index(2, 1) == 5u);                                   // track 3, INS B
  CHECK(core::insert_param_index(model::kInsertWhereMaster, 0) == core::kInsertIdMaster);  // the master: 32
  CHECK(core::param_index(ins(model::kInsertWhereMaster, 0, 3)) == 32u);
  // no record, no parameter
  CHECK_FALSE(p.has_param(ins(1, 0, 0)));
  CHECK(p.get_param(ins(1, 0, 0)) == 0.0f);
  // a track's insert B (where 5, slot 1) with three parameters; the master's insert with two
  model::InsertRecord r{};
  r.where = 5;
  r.slot = 1;
  r.module = 0;
  r.n = 3;
  r.param[0] = 0.25f;
  r.param[1] = -2.0f;
  r.param[2] = 7.5f;
  CHECK(p.set_insert(r) == model::Status::Ok);
  model::InsertRecord m{};
  m.where = model::kInsertWhereMaster;
  m.slot = 0;
  m.module = 0;
  m.n = 2;
  m.param[0] = 1.5f;
  m.param[1] = 3.0f;
  CHECK(p.set_insert(m) == model::Status::Ok);
  CHECK(p.has_param(ins(5, 1, 0)));
  CHECK(p.get_param(ins(5, 1, 2)) == 7.5f);
  CHECK_FALSE(p.has_param(ins(5, 1, 3)));  // past the record's n
  CHECK(p.get_param(ins(5, 1, 3)) == 0.0f);
  CHECK_FALSE(p.has_param(ins(5, 0, 0)));  // the other slot of the track has no record
  CHECK(p.has_param(core::param_id(ParamKind::Insert, 32, 1)));  // the master by its id index
  CHECK(p.get_param(core::param_id(ParamKind::Insert, 32, 1)) == 3.0f);
  CHECK_FALSE(p.has_param(core::param_id(ParamKind::Insert, 32, 2)));
  CHECK_FALSE(p.has_param(core::param_id(ParamKind::Insert, 33, 0)));  // the master has no slot 1: not an id
  CHECK_FALSE(p.has_param(core::param_id(ParamKind::Insert, 10, 32)));  // field 32 is past O3M_PARAMS_MAX
  // the generation follows the value alone: unchanged for the other parameter and the other insert, different for the one written, equal when put back, different after a load (the epoch)
  const std::uint32_t g0 = p.param_generation(ins(5, 1, 0));
  const std::uint32_t g1 = p.param_generation(ins(5, 1, 1));
  const std::uint32_t gm = p.param_generation(core::param_id(ParamKind::Insert, 32, 0));
  r.param[0] = 0.5f;
  CHECK(p.set_insert(r) == model::Status::Ok);
  CHECK(p.param_generation(ins(5, 1, 0)) != g0);
  CHECK(p.param_generation(ins(5, 1, 1)) == g1);
  CHECK(p.param_generation(core::param_id(ParamKind::Insert, 32, 0)) == gm);
  r.param[0] = 0.25f;
  CHECK(p.set_insert(r) == model::Status::Ok);
  CHECK(p.param_generation(ins(5, 1, 0)) == g0);
  p.reset();
  CHECK_FALSE(p.has_param(ins(5, 1, 0)));  // a reset leaves no insert
  CHECK(p.param_generation(ins(5, 1, 0)) != g0);
  // set_param does not write an insert: it is written by set_insert (not an undo unit)
  CHECK(p.set_param(ins(5, 1, 0), 1.0f) == model::Status::BadIndex);
  CHECK(model::param_desc(ins(5, 1, 0)) == nullptr);  // the descriptor is the bound module's, not a table row
  model::ParamField f;
  CHECK_FALSE(model::find_param(ins(5, 1, 0), f));
  CHECK(model::param_slots_per_index(ParamKind::Insert) == 0u);
  CHECK(model::param_index_count(ParamKind::Insert) == 0u);
}

TEST_CASE("native settings: an insert's Learn binding (OS-INSFX S7) survives a save and a load, and what this version has no id for is skipped and counted, as an older reader skips every Insert entry") {
  setup_with_settings();
  const auto ins = [](unsigned index, unsigned field) { return core::param_id(ParamKind::Insert, index, field); };
  model::Project& p = project();
  CHECK(p.set_learn_slot(3, core::LearnBinding{ins(2, 0), 20, 0, 0, 0}) == model::Status::Ok);                       // T2 INS A, field 0 (where 2)
  CHECK(p.set_learn_slot(4, core::LearnBinding{ins(core::kInsertIdMaster, 31), 21, 3, 1, 0}) == model::Status::Ok);  // the master insert's last field, channel 3, jump
  CHECK(p.set_learn_slot(5, core::LearnBinding{ins(33, 0), 22, 0, 0, 0}) == model::Status::BadValue);                // the master has no slot 1
  CHECK(p.set_learn_slot(5, core::LearnBinding{ins(0, 32), 22, 0, 0, 0}) == model::Status::BadValue);                // field 32
  CHECK(p.set_learn_slot(5, core::LearnBinding{ins(2, 1), 22, 0, 0, core::kLearnFollow}) == model::Status::BadValue);  // an insert never follows
  model::Settings device;
  device.init(ParamKind::Device);
  CHECK(device.set_learn(0, core::LearnBinding{ins(2, 0), 20, 0, 0, 0}) == model::Status::BadValue);  // the project's store, not the device's
  const Bytes a = save();
  const Snapshot before = take();
  const model::Settings settings_before = p.settings();
  p.reset();
  const formats::native::Report r = load(a);
  CHECK(r.error == formats::native::Error::None);
  CHECK(r.settings_skipped == 0u);
  CHECK(equal(take(), before));
  CHECK(project().settings().learn_count() == 5u);
  CHECK(std::memcmp(project().settings().learn_slots(), settings_before.learn_slots(), core::kLearnSlots * sizeof(core::LearnBinding)) == 0);
  CHECK(project().check_invariants());
  CHECK(save() == a);
  // The rule an older reader applies to these entries: a kind it has no binding for is skipped and counted, the project opens with the rest. A kind 7 stands for it here (the older reader's
  // bindable kinds were Instrument .. Device); the two bindings of an Insert id this version cannot have (index 33, field 32) are skipped by this reader's own check.
  std::vector<Chunk> cs = chunks_of(a);
  Chunk& l = find(cs, kLrnb);
  NEED(l.payload.size() == 4u + 5u * 8u);
  u32_at(l.payload, 4 + 3 * 8, static_cast<std::uint32_t>(core::param_id(static_cast<ParamKind>(7), 2, 0)));  // entry 3: a kind of a later version
  u32_at(l.payload, 4 + 4 * 8, static_cast<std::uint32_t>(ins(33, 0)));                                          // entry 4: index 33
  const formats::native::Report r2 = load(rebuild(cs));
  CHECK(r2.error == formats::native::Error::None);
  CHECK(r2.settings_skipped == 2u);
  CHECK(project().settings().learn_count() == 3u);
  CHECK(project().check_invariants());
}

TEST_CASE("native settings: what a file lacks is the default, what it carries that this version cannot use is skipped and counted") {
  setup_with_settings();
  std::vector<Chunk> cs = chunks_of(save());
  // No SETS and no LRNB: the project's defaults.
  std::vector<Chunk> bare;
  for (const Chunk& c : cs) {
    if (c.id != kSets && c.id != kLrnb) {
      bare.push_back(c);
    }
  }
  formats::native::Report r = load(rebuild(bare));
  CHECK(r.error == formats::native::Error::None);
  CHECK(project().settings().is_default());
  CHECK(project().instrument(2) != nullptr);  // the rest of the project is there
  // A SETS of one entry (an older file): the other settings keep their defaults.
  std::vector<Chunk> older = cs;
  Chunk& sets = find(older, kSets);
  NEED(sets.payload.size() == 28u);
  sets.payload.resize(4u + 8u);
  sets.payload[3] = 1;  // count 1: the first entry, ANTI-ALIAS, which was written as 1
  r = load(rebuild(older));
  CHECK(r.error == formats::native::Error::None);
  CHECK(project().settings().get(model::project_setting::SendsMode) == 0.0f);
  // Entries of a newer file: an unknown setting, a value out of range, NaN, a setting of the device's scope: all skipped, counted.
  std::vector<Chunk> foreign = cs;
  Chunk& fsets = find(foreign, kSets);
  u32_at(fsets.payload, 4 + 0 * 8, static_cast<std::uint32_t>(core::param_id(ParamKind::Device, 0, 3)));  // entry 0: a device setting
  u32_at(fsets.payload, 4 + 1 * 8, static_cast<std::uint32_t>(setting(40)));          // entry 1: no such setting
  u32_at(fsets.payload, 4 + 2 * 8 + 4, 0x40000000u);                                      // entry 2: the value 2.0
  r = load(rebuild(foreign));
  CHECK(r.error == formats::native::Error::None);
  CHECK(r.settings_skipped == 3u);
  CHECK(project().settings().get(model::project_setting::AntiAliasing) == 1.0f);
  CHECK(project().settings().get(model::project_setting::SendsMode) == 0.0f);
  CHECK(project().settings().get(model::project_setting::LimiterMode) == 0.0f);
  // Bindings: a mixer parameter (the device's store), a bad controller, a bad mode: skipped, counted; the good ones are read.
  std::vector<Chunk> odd = cs;
  Chunk& l = find(odd, kLrnb);
  NEED(l.payload.size() == 4u + 3u * 8u);
  u32_at(l.payload, 4, static_cast<std::uint32_t>(track(0, 0)));  // entry 0: a track's volume
  l.payload[4 + 8 + 4] = 200;                                      // entry 1: controller 200
  r = load(rebuild(odd));
  CHECK(r.error == formats::native::Error::None);
  CHECK(r.settings_skipped == 2u);
  CHECK(project().settings().learn_count() == 1u);
  CHECK(project().settings().learn(0).flags == core::kLearnFollow);
  CHECK(project().check_invariants());
}

TEST_CASE("native settings: order and shape are enforced like every other chunk") {
  setup_with_settings();
  const std::vector<Chunk> cs = chunks_of(save());
  const auto refused = [&](const std::vector<Chunk>& edited, const char* what) {
    const formats::native::Report r = load(rebuild(edited));
    CHECK_MESSAGE(r.error == formats::native::Error::Malformed, what << ": " << formats::native::error_name(r.error) << " " << r.detail);
    CHECK_MESSAGE(project().check_invariants(), what);  // a refused file leaves a valid, empty project
    CHECK_MESSAGE(project().settings().is_default(), what);
  };
  {  // LRNB before SETS
    std::vector<Chunk> e = cs;
    std::swap(e[e.size() - 1], e[e.size() - 2]);
    refused(e, "LRNB before SETS");
  }
  {  // SETS before the instruments
    std::vector<Chunk> e = cs;
    std::swap(e[e.size() - 2], e[e.size() - 3]);
    refused(e, "SETS before INST");
  }
  {  // SETS twice
    std::vector<Chunk> e = cs;
    e.insert(e.end() - 1, find(e, kSets));
    refused(e, "SETS twice");
  }
  {  // a count that does not fit the chunk
    std::vector<Chunk> e = cs;
    find(e, kSets).payload[3] = 5;
    refused(e, "SETS count 5 of 4 entries");
  }
  {
    std::vector<Chunk> e = cs;
    find(e, kLrnb).payload.push_back(0);
    refused(e, "LRNB a byte long");
  }
  {
    std::vector<Chunk> e = cs;
    find(e, kLrnb).payload[3] = 40;
    refused(e, "LRNB 40 bindings");
  }
  {  // a chunk too short to hold its version
    std::vector<Chunk> e = cs;
    find(e, kSets).payload.resize(1);
    refused(e, "SETS one byte");
  }
  {  // a newer major
    std::vector<Chunk> e = cs;
    find(e, kLrnb).payload[0] = 2;
    const formats::native::Report r = load(rebuild(e));
    CHECK(r.error == formats::native::Error::NewerMajor);
  }
  {  // a newer minor with appended bytes is read, and written back without them
    std::vector<Chunk> e = cs;
    find(e, kSets).payload[1] = 3;
    find(e, kSets).payload.insert(find(e, kSets).payload.end(), 50, 0xAB);
    find(e, kLrnb).payload[1] = 1;
    find(e, kLrnb).payload.insert(find(e, kLrnb).payload.end(), 9, 0xCD);
    const formats::native::Report r = load(rebuild(e));
    CHECK(r.error == formats::native::Error::None);
    CHECK(r.newer_minor);
    CHECK(project().settings().get(model::project_setting::SendsMode) == 1.0f);
    CHECK(project().settings().learn_count() == 3u);
    setup_with_settings();
    const Bytes original = save();
    CHECK(load(rebuild(e)).error == formats::native::Error::None);
    CHECK(save() == original);
  }
}

TEST_CASE("native settings: a file cut anywhere, or damaged inside its settings chunks with a good CRC, never leaves an invalid project") {
  setup_with_settings();
  const Bytes good = save();
  for (std::size_t cut = 0; cut < good.size(); cut += kScale) {
    const formats::native::Report r = load(Bytes(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(cut)), 4096);
    CHECK_MESSAGE(r.error == formats::native::Error::Truncated, "cut at " << cut);
    CHECK(project().check_invariants());
  }
  // Damage inside SETS and LRNB (the 36 + 36 bytes before the 14 of the CRC chunk), the CRC made good again.
  const std::size_t from = good.size() - 14 - (8 + 28) - (8 + 28);
  test::Rng rng(271828u);
  unsigned read_ok = 0;
  for (unsigned n = 0; n < 3000 / kScale; ++n) {
    Bytes f = good;
    for (unsigned e = 1 + rng.below(3); e != 0; --e) {
      f[from + rng.below(static_cast<std::uint32_t>(f.size() - 14 - from))] = rng.byte();
    }
    u32_at(f, f.size() - 4, formats::crc32(f.data(), f.size() - 14));
    const formats::native::Report r = load(f);
    CHECK(project().check_invariants());
    if (r.error == formats::native::Error::None) {
      ++read_ok;
      CHECK(project().settings().check_invariants());
      const Bytes again = save();
      CHECK(load(again).error == formats::native::Error::None);
      CHECK(save() == again);
    } else {
      CHECK(project().settings().is_default());
    }
  }
  CHECK(read_ok > 300u / kScale);
}
