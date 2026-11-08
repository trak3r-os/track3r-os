// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSTUI S7: COPY / PASTE / NAME / SAVE / LOAD of the Instrument screen (Shift + F1..F5, the chooser's F3 and F5).
//   * the clipboard and shared audio: a paste shares the source slot's block (no copy, no arena), and both slots stay right when the source is deleted, edited, reloaded or undone, and when
//     the arena's reclaim and the edit pool would have moved or reused the block;
//   * PASTE replaces a used slot, is refused only where a module is in the way, and is one undo unit that gives the slot its audio back;
//   * NAME is an undo unit of the stored field (32 characters);
//   * SAVE writes a .pti of the OG's format into instruments/ through the atomic save, never over a file, behind the card's gate, leaving no partial file; LOAD reads one into an empty slot
//     and refuses a bad or short file cleanly.
#include <doctest/doctest.h>

#include "support/mem_card.h"
#include "support/need.h"
#include "support/og_fixture.h"

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/buttons.h>
#include <o3os/app/pti_load.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/save_service.h>
#include <o3os/app/screens/params_screen.h>
#include <o3os/app/text_entry.h>
#include <o3os/audio/voices.h>
#include <o3os/core/input_event.h>
#include <o3os/formats/crc32.h>
#include <o3os/formats/og.h>
#include <o3os/model/editor.h>
#include <o3os/model/params.h>
#include <o3os/platform/host/fakes.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace o3os;
using app::Button;
using app::DispatchResult;
using core::ActionArg;
using test::MemCard;
using Bytes = MemCard::Bytes;

namespace {

alignas(8) std::uint8_t g_ring[256 * 1024];

constexpr unsigned kA = 4;       // instrument 5: KICK, a sample instrument with audio
constexpr unsigned kB = 6;       // instrument 7: SNARE, another one
constexpr unsigned kMidi = 8;    // instrument 9: a MIDI instrument on channel 3
constexpr unsigned kFree = 11;   // instrument 12: empty
constexpr unsigned kFree2 = 12;  // instrument 13: empty
constexpr unsigned kHigh = 60;   // instrument 61: empty, in the 49..64 range
constexpr unsigned kModule = 14; // instrument 15: a module instrument
constexpr std::uint32_t kFramesA = 8000;
constexpr std::uint32_t kFramesB = 6000;

std::int16_t sample_at(std::uint32_t seed, std::uint32_t i) { return static_cast<std::int16_t>(((i * 37u + seed * 101u) % 2000u) * 10u - 10000); }
void fill(std::int16_t* p, std::uint32_t n, std::uint32_t seed) {
  for (std::uint32_t i = 0; i < n; ++i) {
    p[i] = sample_at(seed, i);
  }
}
bool holds(const std::int16_t* p, std::uint32_t n, std::uint32_t seed) {
  if (p == nullptr) {
    return false;
  }
  for (std::uint32_t i = 0; i < n; ++i) {
    if (p[i] != sample_at(seed, i)) {
      return false;
    }
  }
  return true;
}

struct Rig {
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<audio::VoicePool> voices = std::make_unique<audio::VoicePool>();
  std::unique_ptr<model::ModelArena> arena = std::make_unique<model::ModelArena>();
  std::vector<std::int16_t> memory = std::vector<std::int16_t>(200000);
  std::vector<std::int16_t> peaks = std::vector<std::int16_t>(20000);
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  std::unique_ptr<app::SampleBankMem> mem = std::make_unique<app::SampleBankMem>();
  app::SampleBank bank;
  MemCard card;
  app::SaveService saves;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);

  Rig() {
    arena->project.reset();
    bank.init(*voices, *arena, *mem, memory.data(), static_cast<std::uint32_t>(memory.size()), peaks.data(), peaks.size());
    add_sample(kA, "KICK", kFramesA, 1);
    add_sample(kB, "SNARE", kFramesB, 2);
    model::Instrument m;
    model::init_instrument(m, model::InstrumentKind::Midi);
    std::strcpy(m.name, "KEYS");
    m.midi_channel = 3;
    NEED(arena->project.set_instrument(kMidi, m) == model::Status::Ok);
    NEED(arena->project.add_module("tb303", 5, 1, 0) == model::Status::Ok);
    model::Instrument mod;
    model::init_instrument(mod, model::InstrumentKind::Module);
    std::strcpy(mod.name, "ACID");
    mod.module_index = 0;
    NEED(arena->project.set_instrument(kModule, mod) == model::Status::Ok);
    journal.begin(g_ring, sizeof g_ring);
    editor = std::make_unique<model::Editor>(arena->project, journal);
    app->attach_document(editor.get());
    app->attach_samples(&bank);
    app->attach_storage(&card);
    saves.attach(card, card);
    app->attach_saves(&saves);
    select(kA);
    tap(Button::Params);
  }
  // OS-BUG-PTIRATE: the recorded rate of a sample instrument (a .pti plays at 44,100 Hz: the save refuses what is not).
  void set_rate(unsigned slot, std::uint32_t rate) {
    model::Instrument in = *arena->project.instrument(slot);
    in.sample_rate = rate;
    NEED(arena->project.set_instrument(slot, in) == model::Status::Ok);
  }
  void add_sample(unsigned slot, const char* name, std::uint32_t frames, std::uint32_t seed) {
    model::Instrument in;
    model::init_instrument(in, model::InstrumentKind::Sample);
    std::strncpy(in.name, name, sizeof in.name - 1);
    std::strcpy(in.sample_path, "samples/instr00.wav");
    in.sample_path[13] = static_cast<char>('0' + (slot + 1) / 10);
    in.sample_path[14] = static_cast<char>('0' + (slot + 1) % 10);
    in.sample_rate = 22050;
    in.sampler.start = 1000;
    in.sampler.end = 60000;
    NEED(arena->project.set_instrument(slot, in) == model::Status::Ok);
    std::uint32_t cap = 0;
    std::int16_t* p = bank.reserve(cap);
    NEED(p != nullptr);
    fill(p, frames, seed);
    NEED(bank.commit_load(slot, p, frames));
    bank.rebind_now(slot);
  }
  void pass(unsigned ms = 16) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  void press(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), true)); }
  void release(Button b) { input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), false)); }
  void tap(Button b) {
    press(b);
    pass(1);
    release(b);
    pass(1);
  }
  void shift_tap(Button b) {
    press(Button::Shift);
    pass(1);
    tap(b);
    release(Button::Shift);
    pass(1);
  }
  DispatchResult invoke(const char* name, ActionArg arg = 0) { return app->invoke(core::action_id(name), arg); }
  void select(unsigned slot) {
    const_cast<app::ViewState&>(app->state()).instrument = static_cast<int>(slot) + 1;
    pass();
  }
  const model::Instrument* inst(unsigned slot) const { return arena->project.instrument(slot); }
  std::string note() const { return app->state().notify.empty() ? std::string() : std::string(app->state().notify.head().text.c_str()); }
  void clear_notes() {
    pass(10000);
    pass(10000);
  }
  std::size_t depth() const { return editor->journal().depth(); }
  void settle() {  // the bank's quiet handover, as the App's pass does it
    std::uint64_t now = 1000000;
    for (int i = 0; i < 4 && bank.rebind_pending(); ++i) {
      (void)bank.poll(now, true, true, true);
      now += 100;
    }
  }
  void copy(unsigned slot) {
    select(slot);
    clear_notes();
    shift_tap(Button::F2);
  }
  void paste(unsigned slot) {
    select(slot);
    clear_notes();
    shift_tap(Button::F3);
  }
  void undo() { (void)invoke("edit.undo"); }
  void redo() { (void)invoke("edit.redo"); }
  app::SampleBank::Pcm pcm(unsigned slot) const { return bank.pcm(slot); }
  // Passes until the jobs have ended.
  void jobs(int max_passes = 400) {
    for (int i = 0; i < max_passes && (app->state().notify.empty() || true); ++i) {
      pass(5);
      if (app->session().jobs.running() == 0u && i > 3) {
        break;
      }
    }
    pass();
  }
  // Types `text` on the open keyboard: the pad under the key.
  void type(const char* text) {
    for (const char* c = text; *c != '\0'; ++c) {
      unsigned key = app::kTextKeys;
      for (unsigned k = 0; k < app::kTextKeys; ++k) {
        if (app::kTextKeysSmall[k] == *c || app::kTextKeysBig[k] == *c) {
          key = k;
          break;
        }
      }
      NEED(key < app::kTextKeys);
      unsigned pad = 48;
      for (unsigned p = 0; p < 48; ++p) {
        if (app::kTextPadKey[p] == key) {
          pad = p;
          break;
        }
      }
      NEED(pad < 48);
      if (app::kTextKeysBig[key] == *c && app::kTextKeysSmall[key] != *c && !app->state().text.entry.shift()) {
        (void)invoke("text.caps");
      }
      (void)invoke("text.pad", static_cast<ActionArg>(pad));
    }
  }
};

}  // namespace

// ---- the clipboard and shared audio ---------------------------------------------------------------------------------------------------------------

TEST_CASE("clip: COPY then PASTE into an empty slot shares the audio: no copy, no arena, one undo unit, a version of its own") {
  Rig rig;
  const std::uint32_t free_before = rig.bank.free_samples();
  const std::uint32_t version_a = rig.bank.version(kA);
  const std::uint32_t version_b = rig.bank.version(kFree);
  rig.copy(kA);
  CHECK(rig.note() == "INS 05 COPIED");
  CHECK(rig.bank.clip_valid());
  const std::size_t depth = rig.depth();
  rig.paste(kFree);
  CHECK(rig.note() == "INS 12 PASTED - F8 UNDO");
  NEED(rig.inst(kFree) != nullptr);
  CHECK(std::string(rig.inst(kFree)->name) == "KICK");
  CHECK(rig.inst(kFree)->kind == model::InstrumentKind::Sample);
  CHECK(std::string(rig.inst(kFree)->sample_path) == "samples/instr12.wav");  // its own file name: two instruments never name one file
  CHECK(rig.inst(kFree)->sample_rate == 22050u);
  CHECK(rig.depth() == depth + 1);
  CHECK(rig.pcm(kFree).data == rig.pcm(kA).data);  // the same block ...
  CHECK(rig.pcm(kFree).frames == kFramesA);
  CHECK(rig.bank.free_samples() == free_before);   // ... no arena
  CHECK(rig.bank.pcm_refs(rig.pcm(kA).data) == 4u);  // A, the paste, the clipboard and the swap record of the paste (the audio the slot has after it)
  CHECK(rig.bank.version(kFree) != version_b);     // the peak cache of the new slot is stale
  CHECK(rig.bank.version(kA) == version_a);        // the source's is not
  CHECK(rig.bank.rebind_pending());
  rig.settle();
  CHECK_FALSE(rig.bank.rebind_pending());
  CHECK(rig.voices->slot_in_use(static_cast<unsigned>(kFree)) == false);
  // The engine's descriptor of the new slot reads the shared block.
  CHECK(holds(rig.pcm(kFree).data, kFramesA, 1));
}

TEST_CASE("clip: undo of a paste gives the slot its audio back (empty slot, then a used one), redo shares again, the versions move each time") {
  Rig rig;
  rig.copy(kA);
  rig.paste(kFree);
  const std::uint32_t v_paste = rig.bank.version(kFree);
  rig.undo();
  CHECK(rig.inst(kFree) == nullptr);
  CHECK(rig.pcm(kFree).data == nullptr);
  CHECK(rig.bank.version(kFree) != v_paste);
  const std::uint32_t v_undo = rig.bank.version(kFree);
  rig.redo();
  NEED(rig.inst(kFree) != nullptr);
  CHECK(rig.pcm(kFree).data == rig.pcm(kA).data);
  CHECK(rig.bank.version(kFree) != v_undo);
  rig.settle();

  // A used slot: SNARE is replaced by KICK; undo gives SNARE its own block and instrument back.
  const std::int16_t* own_b = rig.pcm(kB).data;
  const std::string sample_path_b = rig.inst(kB)->sample_path;
  rig.paste(kB);
  CHECK(rig.note() == "INS 07 REPLACED - F8 UNDO");
  CHECK(std::string(rig.inst(kB)->name) == "KICK");
  CHECK(rig.pcm(kB).data == rig.pcm(kA).data);
  CHECK(rig.bank.pcm_refs(own_b) >= 1u);  // the swap record keeps the replaced block alive
  rig.undo();
  CHECK(std::string(rig.inst(kB)->name) == "SNARE");
  CHECK(std::string(rig.inst(kB)->sample_path) == sample_path_b);
  CHECK(rig.pcm(kB).data == own_b);
  CHECK(rig.pcm(kB).frames == kFramesB);
  CHECK(holds(rig.pcm(kB).data, kFramesB, 2));
  CHECK(holds(rig.pcm(kA).data, kFramesA, 1));  // the source never moved
  rig.redo();
  CHECK(std::string(rig.inst(kB)->name) == "KICK");
  CHECK(rig.pcm(kB).data == rig.pcm(kA).data);
  rig.settle();
}

TEST_CASE("clip: the source deleted, then undone, leaves the paste right") {
  Rig rig;
  rig.copy(kA);
  rig.paste(kFree);
  rig.select(kA);
  rig.clear_notes();
  rig.shift_tap(Button::F4);  // DELETE
  CHECK(rig.inst(kA) == nullptr);
  rig.settle();
  CHECK(holds(rig.pcm(kFree).data, kFramesA, 1));
  CHECK(rig.inst(kFree) != nullptr);
  rig.undo();  // the delete
  NEED(rig.inst(kA) != nullptr);
  CHECK(holds(rig.pcm(kA).data, kFramesA, 1));
  CHECK(holds(rig.pcm(kFree).data, kFramesA, 1));
  // The clipboard is still the copy, and pastes again.
  rig.paste(kFree2);
  CHECK(rig.pcm(kFree2).data == rig.pcm(kFree).data);
}

TEST_CASE("clip: the source edited (and the edit pool cycled) leaves the paste and the clipboard on the old block") {
  Rig rig;
  rig.copy(kA);
  rig.paste(kFree);
  const std::int16_t* shared = rig.pcm(kA).data;
  // Edit A over and over: every edit writes a buffer of the pool, and the pool reuses a buffer only when nothing holds it.
  for (unsigned round = 0; round < 8; ++round) {
    std::int16_t* buf = rig.bank.edit_begin(kA, kFramesA);
    NEED(buf != nullptr);
    CHECK(buf != shared);
    fill(buf, kFramesA, 50 + round);
    NEED(rig.bank.edit_commit(kA, buf, kFramesA, app::PointsRec{}, app::PointsRec{}));
  }
  CHECK(rig.pcm(kA).data != shared);
  CHECK(rig.pcm(kFree).data == shared);
  CHECK(holds(shared, kFramesA, 1));
  rig.settle();
  // The clipboard was taken before the edits: it pastes the old audio.
  rig.paste(kFree2);
  CHECK(rig.pcm(kFree2).data == shared);
  CHECK(holds(rig.pcm(kFree2).data, kFramesA, 1));
  // The sample edit's own undo walks A back through its versions and the shared block is still whole.
  app::PointsRec restore;
  while (rig.bank.edit_undo(kA, restore)) {
  }
  CHECK(holds(shared, kFramesA, 1));
  // An edit of the PASTE gives that slot a block of its own and leaves A and the clipboard alone.
  std::int16_t* buf = rig.bank.edit_begin(kFree, kFramesA);
  NEED(buf != nullptr);
  fill(buf, kFramesA, 99);
  NEED(rig.bank.edit_commit(kFree, buf, kFramesA, app::PointsRec{}, app::PointsRec{}));
  CHECK(rig.pcm(kFree).data == buf);
  CHECK(holds(shared, kFramesA, 1));
  CHECK(rig.pcm(kFree2).data == shared);
}

TEST_CASE("clip: COPY takes the instrument as it is NOW: later edits of the source are not in the paste") {
  Rig rig;
  rig.copy(kA);
  // the source's volume changes after the copy
  model::Instrument edited = *rig.inst(kA);
  edited.sampler.volume = 7;
  NEED(rig.editor->set_instrument(kA, edited) == model::Status::Ok);
  rig.paste(kFree);
  NEED(rig.inst(kFree) != nullptr);
  CHECK(rig.inst(kFree)->sampler.volume == 50);  // the copy's, not the edit's
  CHECK(rig.inst(kA)->sampler.volume == 7);
}

TEST_CASE("clip: reclaim never moves or overwrites a block another holder reads") {
  // SNARE (slot 7) owns the last block of the arena: loading new audio into it right behind is what reclaim wins back (OS-FU1), unless somebody else reads the block.
  Rig plain;
  std::uint32_t c2 = 0;
  std::int16_t* q = plain.bank.reserve(c2);
  NEED(q != nullptr);
  fill(q, 1000, 5);
  NEED(plain.bank.commit_load(kB, q, 1000));
  plain.settle();
  CHECK(plain.bank.reclaims() == 1u);  // nothing shared: the old block's room is won back

  Rig rig;
  rig.copy(kB);
  rig.paste(kFree);
  const std::int16_t* shared = rig.pcm(kB).data;
  rig.settle();
  const std::uint32_t reclaims = rig.bank.reclaims();
  std::uint32_t cap = 0;
  std::int16_t* p = rig.bank.reserve(cap);
  NEED(p != nullptr);
  fill(p, 3000, 70);
  NEED(rig.bank.commit_load(kB, p, 3000));
  rig.settle();
  CHECK(rig.bank.reclaims() == reclaims);  // the old block is read by the paste and the clipboard: kept
  CHECK(holds(shared, kFramesB, 2));
  CHECK(holds(rig.pcm(kFree).data, kFramesB, 2));
  CHECK(holds(rig.pcm(kB).data, 3000, 70));
  // The other way round: the new audio is copied and pasted, then a still newer load into SNARE must not move it from under its holders.
  rig.copy(kB);
  rig.paste(kFree2);
  const std::int16_t* second = rig.pcm(kB).data;
  p = rig.bank.reserve(cap);
  NEED(p != nullptr);
  fill(p, 2000, 71);
  NEED(rig.bank.commit_load(kB, p, 2000));
  rig.settle();
  CHECK(rig.bank.reclaims() == reclaims);
  CHECK(holds(second, 3000, 70));
  CHECK(holds(rig.pcm(kFree2).data, 3000, 70));
  CHECK(holds(rig.pcm(kB).data, 2000, 71));
}

TEST_CASE("clip: more than 16 swaps: undo and redo across the overflow keep the pairing (a unit whose swap was pushed out leaves the audio alone and takes no newer swap)") {
  Rig rig;
  const std::int16_t* block_a = rig.pcm(kA).data;
  const std::int16_t* block_b = rig.pcm(kB).data;
  constexpr unsigned kUnits = 20;  // 16 swaps are kept: the 4 oldest units lose theirs
  for (unsigned i = 1; i <= kUnits; ++i) {
    rig.copy(i % 2 == 1 ? kA : kB);
    rig.paste(kFree);
    CHECK(rig.pcm(kFree).data == (i % 2 == 1 ? block_a : block_b));
  }
  for (unsigned i = kUnits; i >= 1; --i) {
    rig.undo();
    if (i > 4) {  // the swap exists: the audio before the unit
      CHECK_MESSAGE(rig.pcm(kFree).data == (i % 2 == 1 ? block_b : block_a), "undo of unit " << i);
    } else if (i > 1) {
      CHECK_MESSAGE(rig.pcm(kFree).data == block_b, "undo of unit " << i << " (swap gone: the audio stays)");
    }
  }
  CHECK(rig.inst(kFree) == nullptr);
  CHECK(rig.pcm(kFree).data == block_b);  // the audio of unit 4, kept
  // The undone stack holds 16 entries too, marks included: the four marks of the oldest units (the first to be redone) took the places of the four newest swaps (units 17..20), which are marks in
  // their turn. A unit with a mark leaves the audio alone: it never takes a swap that is not its own.
  const std::int16_t* held = block_b;
  for (unsigned i = 1; i <= kUnits; ++i) {
    rig.redo();
    const bool marked = i <= 4 || i >= 17;
    if (!marked) {
      held = i % 2 == 1 ? block_a : block_b;
    }
    CHECK_MESSAGE(rig.pcm(kFree).data == held, "redo of unit " << i << (marked ? " (a mark: the audio stays)" : ""));
  }
  CHECK(rig.pcm(kFree).data == block_b);
  CHECK(std::string(rig.inst(kFree)->name) == "SNARE");
}

TEST_CASE("clip: an edit that is not a paste discards the redo units, and the swaps of the undone ones stop keeping their blocks alive") {
  Rig rig;
  rig.copy(kA);
  std::int16_t* n = rig.bank.edit_begin(kA, kFramesA);
  NEED(n != nullptr);
  fill(n, kFramesA, 77);
  NEED(rig.bank.edit_commit(kA, n, kFramesA, app::PointsRec{}, app::PointsRec{}));
  const std::int16_t* p0 = rig.bank.clip_pcm().data;  // the copy-time block, no longer A's
  rig.paste(kFree);
  rig.copy(kMidi);  // the clipboard lets go of it
  CHECK(rig.pcm(kFree).data == p0);
  rig.undo();       // the paste: its swap is on the undone stack, holding p0
  CHECK(rig.pcm(kFree).data == nullptr);
  CHECK(rig.bank.pcm_refs(p0) == 1u);
  (void)rig.invoke("project.tempo", 1);  // a new unit: the redo of the paste is gone
  CHECK(rig.bank.pcm_refs(p0) == 0u);
  rig.redo();  // nothing to redo
  CHECK(rig.pcm(kFree).data == nullptr);
}

TEST_CASE("clip: a project load empties the clipboard (O6)") {
  Rig rig;
  rig.copy(kA);
  CHECK(rig.bank.clip_valid());
  rig.bank.clear();
  CHECK_FALSE(rig.bank.clip_valid());
  CHECK(rig.bank.pcm_refs(nullptr) == 0u);
}

TEST_CASE("clip: PASTE with nothing copied, a module in the way, COPY of a module or an empty slot: each says why and changes nothing") {
  Rig rig;
  const std::size_t depth = rig.depth();
  rig.select(kFree);
  rig.clear_notes();
  rig.shift_tap(Button::F3);
  CHECK(rig.note() == "NOTHING TO PASTE: SHIFT+F2 COPIES");
  rig.clear_notes();
  rig.shift_tap(Button::F2);
  CHECK(rig.note() == "NOTHING TO COPY");
  rig.copy(kModule);
  CHECK(rig.note() != "A MODULE INSTRUMENT CANNOT BE COPIED");  // OS-MODSENDS: its Shift + F2 is MIX now; nothing is copied
  CHECK(rig.app->state().page[app::screen_index(app::ScreenId::Instrument)] == app::ParamsScreen::kMixPage);
  CHECK_FALSE(rig.bank.clip_valid());
  rig.copy(kA);
  rig.paste(kModule);
  CHECK(rig.note() == "INS 15 IS A MODULE: DELETE IT FIRST");
  CHECK(rig.inst(kModule)->kind == model::InstrumentKind::Module);
  CHECK(rig.depth() == depth);
  // After its delete the slot takes the paste (the instance stays bound but is not routed: S5).
  rig.clear_notes();
  rig.shift_tap(Button::F4);
  CHECK(rig.inst(kModule) == nullptr);
  rig.paste(kModule);
  NEED(rig.inst(kModule) != nullptr);
  CHECK(rig.inst(kModule)->kind == model::InstrumentKind::Sample);
}

TEST_CASE("mix: a module instrument's Shift+F2 opens MIX, whose PANNING and sends edit, undo and reach the descriptor (OS-MODSENDS)") {
  Rig rig;
  rig.copy(kModule);
  const unsigned page = rig.app->state().page[app::screen_index(app::ScreenId::Instrument)];
  NEED(page == app::ParamsScreen::kMixPage);
  const auto sp = [&]() -> const model::SamplerParams& { return rig.inst(kModule)->sampler; };
  CHECK(sp().panning == 50);
  CHECK(sp().delay_send == 0);
  CHECK(sp().reverb_send == 0);
  const std::size_t depth = rig.depth();
  const auto turn = [&](int detents) {
    rig.input.push(core::make_encoder_event(rig.clock.now(), 0, static_cast<std::int16_t>(detents)));
    rig.pass();
  };
  turn(10);  // row 0: PANNING
  CHECK(sp().panning > 50);
  rig.tap(Button::Down);
  turn(10);  // row 1: DELAY SEND
  CHECK(sp().delay_send > 0);
  rig.tap(Button::Down);
  turn(10);  // row 2: REVERB SEND
  CHECK(sp().reverb_send > 0);
  CHECK(rig.depth() > depth);
  rig.settle();
  CHECK(rig.voices->sample(kModule).pan == sp().panning);  // the rebind hands the descriptor the edit
  CHECK(rig.voices->sample(kModule).delay_send == sp().delay_send);
  CHECK(rig.voices->sample(kModule).reverb_send == sp().reverb_send);
  for (int i = 0; i < 6 && rig.depth() > depth; ++i) {
    rig.undo();
  }
  CHECK(sp().panning == 50);
  CHECK(sp().delay_send == 0);
  CHECK(sp().reverb_send == 0);
  CHECK(rig.depth() == depth);
  rig.settle();
  CHECK(rig.voices->sample(kModule).pan == 50);
  CHECK(rig.voices->sample(kModule).reverb_send == 0);
  // a sample instrument's Shift+F2 is COPY still
  rig.copy(kA);
  CHECK(rig.bank.clip_valid());
}

TEST_CASE("mix: a second Shift+F2 leaves MIX, and another slot does not open on it (OS-MODSENDS)") {
  Rig rig;
  const auto page = [&]() -> unsigned { return rig.app->state().page[app::screen_index(app::ScreenId::Instrument)]; };
  rig.copy(kModule);
  NEED(page() == app::ParamsScreen::kMixPage);
  rig.copy(kModule);  // the toggle: back to the module's first page, for every module (one with no parameters has no page key)
  CHECK(page() == 0u);
  rig.copy(kModule);
  NEED(page() == app::ParamsScreen::kMixPage);
  CHECK(rig.invoke("instrument.step", 1) == DispatchResult::Changed);  // F7: another slot opens on its own first page
  CHECK(page() == 0u);
  rig.copy(kModule);
  NEED(page() == app::ParamsScreen::kMixPage);
  CHECK(rig.invoke("instrument.step", -1) == DispatchResult::Changed);  // F6
  CHECK(page() == 0u);
}

TEST_CASE("clip: a MIDI instrument pastes with its channel, over a sample slot it leaves the audio for the undo") {
  Rig rig;
  rig.copy(kMidi);
  rig.paste(kHigh);
  NEED(rig.inst(kHigh) != nullptr);
  CHECK(rig.inst(kHigh)->kind == model::InstrumentKind::Midi);
  CHECK(rig.inst(kHigh)->midi_channel == 3);
  CHECK(std::string(rig.inst(kHigh)->name) == "KEYS");
  const std::int16_t* own = rig.pcm(kB).data;
  rig.paste(kB);  // over SNARE
  CHECK(rig.inst(kB)->kind == model::InstrumentKind::Midi);
  rig.settle();
  rig.undo();
  CHECK(rig.inst(kB)->kind == model::InstrumentKind::Sample);
  CHECK(rig.pcm(kB).data == own);
  CHECK(holds(rig.pcm(kB).data, kFramesB, 2));
  rig.settle();
}

TEST_CASE("clip: the chooser's F3 pastes into the empty slot and the key is dim until something is copied") {
  Rig rig;
  rig.select(kFree);
  app::ParamsKeyRow row;
  app::params_key_row(rig.app->model(), rig.app->state(), row);
  CHECK(std::string(row.label[2]) == "PASTE");
  CHECK((row.dim & (1u << 2)) != 0u);
  CHECK((row.dim & (1u << 4)) == 0u);  // LOAD is live
  rig.copy(kA);
  rig.select(kFree);
  app::params_key_row(rig.app->model(), rig.app->state(), row);
  CHECK((row.dim & (1u << 2)) == 0u);
  rig.tap(Button::F3);
  NEED(rig.inst(kFree) != nullptr);
  CHECK(std::string(rig.inst(kFree)->name) == "KICK");
  // The page left is the sample instrument's first.
  app::params_key_row(rig.app->model(), rig.app->state(), row);
  CHECK(std::string(row.label[0]) == "SAMPLE");
}

TEST_CASE("clip: the Shift row follows the instrument: COPY and SAVE dim on a module, SAVE dim on MIDI, NAME and DELETE dim on an empty slot") {
  Rig rig;
  const auto dim = [&](unsigned slot) {
    rig.select(slot);
    app::ViewState v = rig.app->state();
    v.shift = true;
    app::ParamsKeyRow row;
    app::params_key_row(rig.app->model(), v, row);
    return row.dim & 0x1Fu;
  };
  CHECK(dim(kA) == 0x04u);           // PASTE: nothing copied
  CHECK(dim(kMidi) == (0x04u | 0x10u));
  CHECK(dim(kModule) == (0x04u | 0x10u));  // COPY is live: on a module it is MIX (OS-MODSENDS)
  CHECK(dim(kFree) == (0x01u | 0x02u | 0x04u | 0x08u | 0x10u));
  rig.copy(kA);
  CHECK(dim(kA) == 0u);
  CHECK(dim(kFree) == (0x01u | 0x02u | 0x08u | 0x10u));  // PASTE is live on an empty slot
}

// ---- NAME ---------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("name: Shift + F1 opens the keyboard on the name, F8 keeps what was typed as one undo unit, F7 leaves it") {
  Rig rig;
  const std::size_t depth = rig.depth();
  rig.shift_tap(Button::F1);
  CHECK(rig.app->state().overlay == app::OverlayId::TextEntry);
  CHECK(rig.app->state().text.target == app::TextEntryView::Target::Instrument);
  CHECK(std::string(rig.app->state().text.entry.text()) == "KICK");
  CHECK(rig.app->state().text.entry.selected());
  rig.tap(Button::F7);  // cancel
  CHECK(rig.app->state().overlay == app::OverlayId::None);
  CHECK(std::string(rig.inst(kA)->name) == "KICK");
  CHECK(rig.depth() == depth);

  rig.shift_tap(Button::F1);
  rig.type("big hat");
  CHECK(std::string(rig.app->state().text.entry.text()) == "big hat");
  rig.tap(Button::F8);
  CHECK(rig.app->state().overlay == app::OverlayId::None);
  CHECK(std::string(rig.inst(kA)->name) == "big hat");
  CHECK(rig.depth() == depth + 1);
  CHECK(rig.note() == "INS 05 RENAMED - F8 UNDO");
  rig.undo();
  CHECK(std::string(rig.inst(kA)->name) == "KICK");
  rig.redo();
  CHECK(std::string(rig.inst(kA)->name) == "big hat");
  // The name of a pasted copy is its own: renaming one does not rename the other.
  rig.copy(kA);
  rig.paste(kFree);
  rig.shift_tap(Button::F1);
  rig.type("x");
  rig.tap(Button::F8);
  CHECK(std::string(rig.inst(kFree)->name) == "x");
  CHECK(std::string(rig.inst(kA)->name) == "big hat");
}

TEST_CASE("name: the stored field decides: 32 characters, the same name records nothing, an empty slot has nothing to name") {
  Rig rig;
  rig.shift_tap(Button::F1);
  rig.type("abcdefghijklmnopqrstuvwxyzabcdefghij");  // 36 typed
  CHECK(rig.app->state().text.entry.length() == model::kInstrumentNameMax);
  rig.tap(Button::F8);
  CHECK(std::string(rig.inst(kA)->name) == "abcdefghijklmnopqrstuvwxyzabcdef");
  const std::size_t depth = rig.depth();
  rig.shift_tap(Button::F1);
  rig.tap(Button::F8);  // accepted as it was
  CHECK(rig.depth() == depth);
  rig.select(kFree);
  rig.clear_notes();
  rig.shift_tap(Button::F1);
  CHECK(rig.app->state().overlay == app::OverlayId::None);
  CHECK(rig.note() == "NO INSTRUMENT TO NAME");
  // The model's call: a name too long or with a NUL is refused, an empty slot is NotFound, an index past the table BadIndex.
  CHECK(rig.editor->set_instrument_name(kA, "x", 1) == model::Status::Ok);
  CHECK(rig.editor->set_instrument_name(kA, std::string(33, 'x').c_str(), 33) == model::Status::BadValue);
  CHECK(rig.editor->set_instrument_name(kA, "a\0b", 3) == model::Status::BadValue);
  CHECK(rig.editor->set_instrument_name(kFree, "x", 1) == model::Status::NotFound);
  CHECK(rig.editor->set_instrument_name(64, "x", 1) == model::Status::BadIndex);
  CHECK(rig.editor->set_instrument_name(kA, "", 0) == model::Status::Ok);
  CHECK(std::string(rig.inst(kA)->name).empty());
}

// ---- SAVE: the .pti ------------------------------------------------------------------------------------------------------------------------------

namespace {

// The bytes the OG writer makes for the instrument in `slot` of `project` with `frames` of `pcm` (standalone: any slot).
Bytes pti_bytes(const model::Project& project, unsigned slot, const std::int16_t* pcm, std::uint32_t frames) {
  struct Sink final : core::ByteSink {
    Bytes out;
    core::IoResult write(const void* src, std::size_t n) noexcept override {
      const auto* b = static_cast<const std::uint8_t*>(src);
      out.insert(out.end(), b, b + n);
      return {n, core::IoStatus::Ok};
    }
  } sink;
  struct Src final : core::ByteSource {
    const std::uint8_t* p;
    std::size_t left;
    core::IoResult read(void* dst, std::size_t max) noexcept override {
      if (left == 0) {
        return {0, core::IoStatus::End};
      }
      const std::size_t n = left < max ? left : max;
      std::memcpy(dst, p, n);
      p += n;
      left -= n;
      return {n, core::IoStatus::Ok};
    }
  } src;
  src.p = reinterpret_cast<const std::uint8_t*>(pcm);
  src.left = 2u * frames;
  auto writer = std::make_unique<formats::og::InstrumentWriter>();
  formats::WriteParams wp;
  wp.index = static_cast<std::uint16_t>(slot);
  wp.pcm = &src;
  wp.pcm_frames = frames;
  wp.standalone = true;
  writer->begin(project, sink, wp);
  for (int i = 0; i < 100000 && writer->progress() == formats::Progress::Running; ++i) {
    (void)writer->step(4096);
  }
  CHECK(writer->progress() == formats::Progress::Done);  // (not NEED: this returns a value)
  return sink.out;
}

// Reads a .pti the way LOAD does (standalone) into `out`; the number of frames the head says follow. False with the reader's error name when it refuses.
bool read_pti(const Bytes& file, unsigned slot, model::Instrument& out, std::uint32_t& frames, std::string* why = nullptr) {
  struct Src final : core::ByteSource {
    const Bytes* b;
    std::size_t at = 0;
    core::IoResult read(void* dst, std::size_t max) noexcept override {
      if (at >= b->size()) {
        return {0, core::IoStatus::End};
      }
      const std::size_t n = b->size() - at < max ? b->size() - at : max;
      std::memcpy(dst, b->data() + at, n);
      at += n;
      return {n, core::IoStatus::Ok};
    }
  } src;
  src.b = &file;
  auto scratch = std::make_unique<model::ModelArena>();
  scratch->project.reset();
  auto reader = std::make_unique<formats::og::InstrumentReader>(true);
  formats::ReadParams rp;
  rp.index = static_cast<std::uint16_t>(slot);
  rp.size_hint = static_cast<std::uint32_t>(file.size());
  rp.standalone = true;
  reader->begin(src, *scratch, rp);
  for (int i = 0; i < 100000 && reader->progress() == formats::Progress::Running; ++i) {
    (void)reader->step(4096);
  }
  CHECK(scratch->project.instrument(slot) == nullptr);  // standalone: the project is not touched
  if (reader->progress() != formats::Progress::Done || reader->import_report().instruments == 0u) {
    if (why != nullptr) {
      *why = reader->report().detail;
    }
    return false;
  }
  out = reader->instrument();
  frames = reader->import_report().pcm_frames;
  return true;
}

}  // namespace

TEST_CASE("pti: write then read on the host, any slot (also 49..64), keeps every field the OG file carries and the audio") {
  Rig rig;
  model::Instrument in = *rig.inst(kA);
  in.sampler.play_mode = 1;
  in.sampler.loop_start = 12000;
  in.sampler.loop_end = 50000;
  in.sampler.volume = 77;
  in.sampler.panning = 20;
  in.sampler.tune = -5;
  in.sampler.fine_tune = 12;
  in.sampler.cutoff = 0.25f;
  in.sampler.resonance = 1.5f;
  in.sampler.filter_enable = 1;
  in.sampler.filter_type = 2;
  in.sampler.envelope[2].attack = 321;
  in.sampler.envelope[2].enable = 1;
  in.sampler.envelope[2].loop = 1;
  in.sampler.lfo[2].shape = 3;
  in.sampler.lfo[2].speed = 9;
  in.sampler.lfo[2].amount = 0.75f;
  in.sampler.slice_count = 2;
  in.sampler.slices[0] = 100;
  in.sampler.slices[1] = 30000;
  in.sampler.reverb_send = 11;
  in.sampler.delay_send = 12;
  in.sampler.overdrive = 13;
  in.sampler.bit_depth = 12;
  NEED(rig.arena->project.set_instrument(kHigh, in) == model::Status::Ok);
  std::vector<std::int16_t> audio(5000);
  fill(audio.data(), 5000, 9);
  const Bytes file = pti_bytes(rig.arena->project, kHigh, audio.data(), 5000);
  CHECK(file.size() == formats::og::kInstrumentBytes + 2u * 5000u);
  model::Instrument back;
  std::uint32_t frames = 0;
  NEED(read_pti(file, kHigh, back, frames));
  CHECK(frames == 5000u);
  const model::SamplerParams& a = in.sampler;
  const model::SamplerParams& b = back.sampler;
  CHECK(std::string(back.name) == "KICK");
  CHECK(b.play_mode == a.play_mode);
  CHECK(b.start == a.start);
  CHECK(b.end == a.end);
  CHECK(b.loop_start == a.loop_start);
  CHECK(b.loop_end == a.loop_end);
  CHECK(b.volume == a.volume);
  CHECK(b.panning == a.panning);
  CHECK(b.tune == a.tune);
  CHECK(b.fine_tune == a.fine_tune);
  CHECK(b.cutoff == a.cutoff);
  CHECK(b.resonance == a.resonance);
  CHECK(b.filter_enable == a.filter_enable);
  CHECK(b.filter_type == a.filter_type);
  CHECK(b.envelope[2].attack == 321);
  CHECK(b.envelope[2].enable == 1);
  CHECK(b.envelope[2].loop == 1);
  CHECK(b.lfo[2].shape == 3);
  CHECK(b.lfo[2].speed == 9);
  CHECK(b.lfo[2].amount == 0.75f);
  CHECK(b.slice_count == 2);
  CHECK(b.slices[1] == 30000);
  CHECK(b.reverb_send == 11);
  CHECK(b.delay_send == 12);
  CHECK(b.overdrive == 13);
  CHECK(b.bit_depth == 12);
  CHECK(std::string(back.sample_path) == "samples/instr61.wav");  // named for the slot it is read for
  // The PCM is what follows the head, byte for byte.
  CHECK(std::memcmp(file.data() + formats::og::kInstrumentBytes, audio.data(), 2u * 5000u) == 0);
  // A second write of the read instrument gives the same bytes (idempotent).
  auto copy = std::make_unique<model::ModelArena>();
  copy->project.reset();
  back.sample_path[0] = '\0';
  std::strcpy(back.sample_path, in.sample_path);
  NEED(copy->project.set_instrument(kHigh, back) == model::Status::Ok);
  CHECK(pti_bytes(copy->project, kHigh, audio.data(), 5000) == file);
}

TEST_CASE("pti: an OG v9 instrument file (the OG fixture of og_fixture.h, a distinct value in every field) reads standalone into slot 50 and writes back byte for byte") {
  Rig rig;
  for (unsigned frames : {1000u, 1u, 600u}) {
    test::og::InstrumentSpec spec;
    spec.frames = frames;
    const Bytes in = test::og::instrument_bytes(spec);
    model::Instrument got;
    std::uint32_t read_frames = 0;
    NEED(read_pti(in, kHigh, got, read_frames));
    CHECK(read_frames == frames);
    CHECK(std::string(got.name) == "Kick 808.wav");
    CHECK(got.sampler.play_mode == 5);
    CHECK(got.sampler.volume == 91);
    CHECK(std::string(got.sample_path) == "samples/instr61.wav");
    auto proj = std::make_unique<model::ModelArena>();
    proj->project.reset();
    NEED(proj->project.set_instrument(kHigh, got) == model::Status::Ok);
    const Bytes pcm(in.begin() + formats::og::kInstrumentBytes, in.end());
    CHECK_MESSAGE(pti_bytes(proj->project, kHigh, reinterpret_cast<const std::int16_t*>(pcm.data()), frames) == in, "frames " << frames);
  }
}

TEST_CASE("pti: the normal export is unchanged: standalone false (the default) writes the bytes it always did, standalone true writes the same ones, and the OG slot limit stays for Export") {
  Rig rig;
  test::og::InstrumentSpec spec;
  spec.frames = 600;
  const Bytes in = test::og::instrument_bytes(spec);
  model::Instrument got;
  std::uint32_t frames = 0;
  NEED(read_pti(in, kB, got, frames));
  auto proj = std::make_unique<model::ModelArena>();
  proj->project.reset();
  NEED(proj->project.set_instrument(kB, got) == model::Status::Ok);
  NEED(proj->project.set_instrument(kHigh, got) == model::Status::Ok);
  const Bytes pcm(in.begin() + formats::og::kInstrumentBytes, in.end());
  const auto run = [&](const model::Project& p, unsigned slot, bool standalone, bool& ok) {
    struct Sink final : core::ByteSink {
      Bytes out;
      core::IoResult write(const void* src, std::size_t n) noexcept override {
        const auto* b = static_cast<const std::uint8_t*>(src);
        out.insert(out.end(), b, b + n);
        return {n, core::IoStatus::Ok};
      }
    } sink;
    struct Src final : core::ByteSource {
      const std::uint8_t* p = nullptr;
      std::size_t left = 0;
      core::IoResult read(void* dst, std::size_t max) noexcept override {
        if (left == 0) {
          return {0, core::IoStatus::End};
        }
        const std::size_t n = left < max ? left : max;
        std::memcpy(dst, p, n);
        p += n;
        left -= n;
        return {n, core::IoStatus::Ok};
      }
    } src;
    src.p = pcm.data();
    src.left = pcm.size();
    auto writer = std::make_unique<formats::og::InstrumentWriter>();
    formats::WriteParams wp;
    wp.index = static_cast<std::uint16_t>(slot);
    wp.pcm = &src;
    wp.pcm_frames = frames;
    if (standalone) {
      wp.standalone = true;
    }
    writer->begin(p, sink, wp);
    for (int i = 0; i < 100000 && writer->progress() == formats::Progress::Running; ++i) {
      (void)writer->step(4096);
    }
    ok = writer->progress() == formats::Progress::Done;
    return sink.out;
  };
  bool ok = false;
  const Bytes normal = run(proj->project, kB, false, ok);
  CHECK(ok);
  CHECK(normal == in);  // the fixture, byte for byte
  const Bytes alone = run(proj->project, kB, true, ok);
  CHECK(ok);
  CHECK(alone == normal);
  (void)run(proj->project, kHigh, false, ok);
  CHECK_FALSE(ok);  // slot 61: not an OG file, Export refuses it
  (void)run(proj->project, kHigh, true, ok);
  CHECK(ok);        // only the library's own save takes it
  // Nothing else reads the flag: the OG project writer and the native writer emit the same bytes whatever it says.
  const auto project_bytes = [&](bool standalone) {
    struct Sink final : core::ByteSink {
      Bytes out;
      core::IoResult write(const void* src, std::size_t n) noexcept override {
        const auto* b = static_cast<const std::uint8_t*>(src);
        out.insert(out.end(), b, b + n);
        return {n, core::IoStatus::Ok};
      }
    } sink;
    auto writer = std::make_unique<formats::og::ProjectWriter>();
    formats::WriteParams wp;
    wp.standalone = standalone;
    writer->begin(rig.arena->project, sink, wp);
    for (int i = 0; i < 100000 && writer->progress() == formats::Progress::Running; ++i) {
      (void)writer->step(4096);
    }
    CHECK(writer->progress() == formats::Progress::Done);
    return sink.out;
  };
  CHECK(project_bytes(false) == project_bytes(true));
}

TEST_CASE("pti: the standalone reader refuses a bad or short file, a slot past the table, stereo audio, and an instrument the model would not take") {
  Rig rig;
  std::vector<std::int16_t> audio(4000);
  fill(audio.data(), 4000, 3);
  const Bytes good = pti_bytes(rig.arena->project, kA, audio.data(), 4000);
  model::Instrument out;
  std::uint32_t frames = 0;
  std::string why;
  CHECK(read_pti(good, 3, out, frames));
  Bytes b(good.begin(), good.begin() + 100);
  CHECK_FALSE(read_pti(b, 3, out, frames, &why));  // shorter than the head
  b = good;
  b[0] = 'X';
  CHECK_FALSE(read_pti(b, 3, out, frames, &why));  // not an OG instrument
  b = good;
  b.resize(good.size() - 1000);  // the head says 4000 frames, the file holds fewer (or: stereo, to the reader)
  CHECK_FALSE(read_pti(b, 3, out, frames, &why));
  b = good;
  b.resize(formats::og::kInstrumentBytes);  // a head and no audio: no instrument
  CHECK_FALSE(read_pti(b, 3, out, frames, &why));
  b = good;
  b[8] = 3;  // a version a .pti never had (the structure version byte)
  CHECK_FALSE(read_pti(b, 3, out, frames, &why));
  CHECK_FALSE(read_pti(good, 64, out, frames, &why));  // slot 65 does not exist
}

namespace {

std::vector<std::string> pti_files(const MemCard& card) {
  std::vector<std::string> out;
  for (const auto& kv : card.files) {
    if (kv.first.rfind("instruments/", 0) == 0) {
      out.push_back(kv.first);
    }
  }
  return out;
}

}  // namespace

TEST_CASE("save: Shift + F5 writes instruments/<name>.pti through the atomic save: the OG writer's bytes, a name that is taken gets a number, nothing is replaced") {
  Rig rig;
  rig.set_rate(kA, 44100);  // a .pti plays at 44,100 Hz (the fixture's 22,050 Hz is refused: the OS-BUG-PTIRATE tests)
  rig.select(kA);
  rig.clear_notes();
  rig.shift_tap(Button::F5);
  rig.jobs();
  const std::vector<std::string> files = pti_files(rig.card);
  NEED(files.size() == 1);
  CHECK(files[0] == "instruments/kick.pti");
  const Bytes expect = pti_bytes(rig.arena->project, kA, rig.pcm(kA).data, kFramesA);
  CHECK(rig.card.same("instruments/KICK.pti", expect));
  CHECK(rig.card.violations == 0u);
  CHECK(rig.card.dirs.count("instruments") == 1u);
  // Read back, it is the instrument (a file of the OG's format, nothing new).
  model::Instrument back;
  std::uint32_t frames = 0;
  NEED(read_pti(*rig.card.get("instruments/kick.pti"), kA, back, frames));
  CHECK(frames == kFramesA);
  CHECK(back.sampler.start == rig.inst(kA)->sampler.start);
  // The same name again: "KICK 2", "KICK 3"; the first file is the bytes it was.
  rig.clear_notes();
  rig.shift_tap(Button::F5);
  rig.jobs();
  rig.clear_notes();
  rig.shift_tap(Button::F5);
  rig.jobs();
  CHECK(pti_files(rig.card).size() == 3u);
  CHECK(rig.card.same("instruments/KICK.pti", expect));
  CHECK(rig.card.same("instruments/KICK 2.pti", expect));
  CHECK(rig.card.same("instruments/KICK 3.pti", expect));
  // No temporary file is left, and the card was only ever asked for guarded paths.
  for (const auto& kv : rig.card.files) {
    CHECK(kv.first.find("_o3save") == std::string::npos);
  }
  CHECK(rig.card.violations == 0u);
  // The user's own file of that name is never touched: a name already on the card is skipped.
  const std::string users = "the user's own file";
  rig.card.put("instruments/SNARE.pti", users);
  rig.set_rate(kB, 44100);
  rig.select(kB);
  rig.clear_notes();
  rig.shift_tap(Button::F5);
  rig.jobs();
  CHECK(rig.card.same("instruments/SNARE.pti", Bytes(users.begin(), users.end())));
  CHECK(rig.card.has("instruments/SNARE 2.pti"));
}

TEST_CASE("save: MIDI instruments say SAMPLE INSTRUMENTS ONLY, a module instrument with no bound module says so (OS-MODPRESET P4: Shift + F5 saves a preset only on a bound module's page), an empty slot and a sample with no audio say so, and nothing reaches the card") {
  Rig rig;
  rig.select(kMidi);
  rig.clear_notes();
  rig.shift_tap(Button::F5);
  CHECK(rig.note() == "SAMPLE INSTRUMENTS ONLY");
  rig.select(kModule);  // (this rig has no module system attached, so the module instrument is not bound: no preset save here)
  rig.clear_notes();
  rig.shift_tap(Button::F5);
  CHECK(rig.note() == "NO MODULE HERE");
  rig.select(kFree);
  rig.clear_notes();
  rig.shift_tap(Button::F5);
  CHECK(rig.note() == "NOTHING TO SAVE");
  // A sample instrument whose slot has no audio.
  model::Instrument in = *rig.inst(kA);
  NEED(rig.arena->project.set_instrument(kFree, in) == model::Status::Ok);
  rig.select(kFree);
  rig.clear_notes();
  rig.shift_tap(Button::F5);
  CHECK(rig.note() == "NO AUDIO IN THIS INSTRUMENT");
  CHECK(rig.card.ops == 0u);
  CHECK(rig.card.files.empty());
  CHECK(rig.card.dirs.empty());
}

TEST_CASE("save: the card's gate refuses a write protected card, and nothing is created") {
  Rig rig;
  rig.set_rate(kA, 44100);
  rig.card.gate_status = hal::storage::WriteStatus::WriteProtected;
  rig.select(kA);
  rig.clear_notes();
  rig.shift_tap(Button::F5);
  CHECK(rig.note() == "CARD IS WRITE PROTECTED");
  rig.jobs();
  CHECK(rig.card.ops == 0u);
  CHECK(rig.card.files.empty());
  CHECK(rig.card.dirs.empty());
  // The card made writable: it goes.
  rig.card.gate_status = hal::storage::WriteStatus::Ok;
  rig.clear_notes();
  rig.shift_tap(Button::F5);
  rig.jobs();
  CHECK(rig.card.has("instruments/kick.pti"));
}

TEST_CASE("save: a write that fails, a card that fills up, a read-back that differs and a power cut leave no .pti behind") {
  for (int how = 0; how < 5; ++how) {
    INFO("fault " << how);
    Rig rig;
    rig.set_rate(kA, 44100);
    switch (how) {
      case 0: rig.card.fail_at = 3; break;                                  // a mutating call in the middle (the temp file's create / a write)
      case 1: rig.card.space_left = 3000; break;                            // the card fills up while the audio is written
      case 2: rig.card.corrupt_readback = true; break;                      // the read-back of the temporary file differs from what was written
      case 3: rig.card.cut_at = 4; break;                                   // the power goes
      case 4: rig.card.short_readback = true; break;
    }
    rig.select(kA);
    rig.clear_notes();
    rig.shift_tap(Button::F5);
    rig.jobs();
    CHECK_MESSAGE(rig.note().find("FAILED") != std::string::npos, "the strip says the save failed: " << rig.note());
    for (const std::string& f : pti_files(rig.card)) {
      CHECK_MESSAGE(f.find("_o3save.tmp") != std::string::npos, "a final file exists: " << f);  // only the temporary name may remain after a cut (the next save removes it)
      if (how != 3) {
        FAIL("no file should remain: " << f);
      }
    }
    CHECK(rig.card.violations == 0u);
    CHECK(rig.card.same("instruments/kick.pti", Bytes{}) == false);
    CHECK_FALSE(rig.card.has("instruments/kick.pti"));
  }
}

TEST_CASE("save: the file name comes from the instrument's name, sanitised (no separators, no .., the guard's names)") {
  const auto stem = [](const char* name, unsigned slot = 0) {
    char out[32];
    app::SaveService::instrument_file_stem(name, slot, out, sizeof out);
    return std::string(out);
  };
  namespace sv = hal::storage::save;
  CHECK(stem("KICK") == "KICK");
  CHECK(stem("Big Hat-2_x.v1") == "Big Hat-2_x.v1");
  CHECK(stem("a/b") == "a_b");
  CHECK(stem("a\\b:c*d?e\"f<g>h|i") == "a_b_c_d_e_f_g_h_i");
  CHECK(stem("../../etc/passwd") == "etc_passwd");
  CHECK(stem("..") == "Instrument 01");
  CHECK(stem("a..b") == "a._b");  // (the second of two dots in a row)
  CHECK(stem("  lead") == "lead");
  CHECK(stem("trail  ") == "trail");
  CHECK(stem("trail.") == "trail");
  CHECK(stem("") == "Instrument 01");
  CHECK(stem("", 59) == "Instrument 60");
  CHECK(stem("@@@") == "Instrument 01");
  CHECK(stem("CON") == "CON_");
  CHECK(stem("nul") == "nul_");
  CHECK(stem("COM1") == "COM1_");
  CHECK(stem("caf\xC3\xA9") == "caf__");
  CHECK(stem("tab\tname") == "tab_name");
  const std::string long_name(32, 'x');
  CHECK(stem(long_name.c_str()).size() == 31u);  // the guard's limit
  for (const char* nasty : {"..", "a/../b", "/", "\\", "CON", "NUL.x", "x\n", "~$%&", "  ", ". .", "aux", "LPT9", "...", "a ."}) {
    INFO("name: " << nasty);
    char out[32];
    app::SaveService::instrument_file_stem(nasty, 7, out, sizeof out);
    CHECK(sv::valid_name(out));
    CHECK(std::string(out).find('/') == std::string::npos);
    CHECK(std::string(out).find("..") == std::string::npos);
  }
  // A small buffer is cut, never overrun.
  char tiny[5];
  app::SaveService::instrument_file_stem("abcdefgh", 0, tiny, sizeof tiny);
  CHECK(std::string(tiny) == "abcd");
}

// ---- LOAD ---------------------------------------------------------------------------------------------------------------------------------------

namespace {

void press_load(Rig& rig, unsigned slot) {
  rig.select(slot);
  rig.clear_notes();
  rig.tap(Button::F5);  // the chooser's LOAD
}

}  // namespace

// OS-BUG-PTIRATE, then OS-FILES-C C1b (maintainer decision Q7, 2026-10-16): a .pti has no rate field and its level is read through the 1.2+ law. What would sound different from the file is
// converted: the audio is resampled to 44,100 Hz (the Amiga engine from Paula's 8,287.14 Hz, its tune kept) and a 1.1.14 level is converted; what has no equivalent is refused with a strip message.
// Nothing is created on the card by a refusal.
TEST_CASE("save: a .pti is written resampled to 44,100 Hz when the instrument plays at another rate, and refused when it has no equivalent (12/8 bit, an Amiga mode Paula does not play, a level under -24 dB)") {
  struct Case {
    const char* what;
    std::uint32_t rate;
    int engine;  // -1: automatic, else audio::kEngine*
    std::uint8_t play_mode;
    std::uint8_t law;
    std::uint8_t level;
    const char* refusal;  // nullptr: the file is written
    int written_level;    // the level the file holds when it is written, -1: the instrument's own
    int resampled_hz;     // the rate the audio is resampled from, 0: written as it is
  };
  const Case cases[] = {
      {"44,100 Hz", 44100, -1, 0, 0, 80, nullptr, 80, 0},
      {"rate 0 (not recorded) is 44,100 Hz", 0, -1, 0, 0, 80, nullptr, 80, 0},
      {"22,050 Hz", 22050, -1, 0, 0, 80, nullptr, 80, 22050},
      {"48,000 Hz", 48000, -1, 1, 0, 80, nullptr, 80, 48000},
      {"16,726 Hz on the Modern engine", 16726, audio::kEngineModern, 0, 0, 80, nullptr, 80, 16726},
      {"a wavetable instrument ignores the rate: written as it is", 22050, -1, 6, 0, 80, nullptr, 80, 0},
      {"the Amiga engine chosen, rate 0 (a MOD sample: period 428, 8,287 Hz at the unity note)", 0, audio::kEngineAmiga, 0, 0, 80, nullptr, 80, 8287},
      {"the Amiga engine chosen, rate 44,100 (the label is not what plays)", 44100, audio::kEngineAmiga, 1, 0, 80, nullptr, 80, 8287},
      {"the Amiga engine by its Paula rate", 8287, -1, 0, 0, 80, nullptr, 80, 8287},
      {"the Amiga engine, a backward loop: Paula has none", 8287, audio::kEngineAmiga, 2, 0, 80, "AMIGA ENGINE: ONE SHOT OR LOOP FWD ONLY", -1, 0},
      {"the Amiga engine, a ping-pong loop", 8287, audio::kEngineAmiga, 3, 0, 80, "AMIGA ENGINE: ONE SHOT OR LOOP FWD ONLY", -1, 0},
      {"the Amiga engine, a slice mode", 8287, audio::kEngineAmiga, 4, 0, 80, "AMIGA ENGINE: ONE SHOT OR LOOP FWD ONLY", -1, 0},
      {"the Amiga engine, a beat slice mode", 8287, audio::kEngineAmiga, 5, 0, 80, "AMIGA ENGINE: ONE SHOT OR LOOP FWD ONLY", -1, 0},
      {"the 12 bit engine, 44,100 Hz", 44100, audio::kEngineBits12, 0, 0, 80, "12/8 BIT ENGINE: NOT SAVED AS A .PTI", -1, 0},
      {"the 8 bit engine, 44,100 Hz", 44100, audio::kEngineBits8, 0, 0, 80, "12/8 BIT ENGINE: NOT SAVED AS A .PTI", -1, 0},
      {"22,050 Hz granular (the rate applies)", 22050, -1, 7, 0, 80, nullptr, 80, 22050},
      {"22,050 Hz slice", 22050, -1, 4, 0, 80, nullptr, 80, 22050},
      {"22,050 Hz beat slice", 22050, -1, 5, 0, 80, nullptr, 80, 22050},
      {"22,050 Hz ping-pong loop", 22050, -1, 3, 0, 80, nullptr, 80, 22050},
      {"22,050 Hz and the 1.1.14 law together: both are converted", 22050, -1, 0, 1, 90, nullptr, 42, 22050},
      {"1.1.14 law, level 100 is 0 dB: 1.2+ level 50", 44100, -1, 0, 1, 100, nullptr, 50, 0},
      {"1.1.14 law, level 90 (-4 dB): 1.2+ level 42", 44100, -1, 0, 1, 90, nullptr, 42, 0},
      {"1.1.14 law, level 40 (-24 dB): 1.2+ level 1", 44100, -1, 0, 1, 40, nullptr, 1, 0},
      {"1.1.14 law, level 39 (-24.4 dB) has no 1.2+ equivalent", 44100, -1, 0, 1, 39, "LEVEL TOO LOW FOR A .PTI (1.1.14 LAW)", -1, 0},
      {"1.1.14 law, level 1 has none either, at 22,050 Hz too", 22050, -1, 0, 1, 1, "LEVEL TOO LOW FOR A .PTI (1.1.14 LAW)", -1, 0},
      {"1.1.14 law, level 0 is silence in both", 44100, -1, 0, 1, 0, nullptr, 0, 0},
      {"the 1.2+ law keeps the level as it is", 44100, -1, 0, 0, 39, nullptr, 39, 0},
  };
  for (const Case& c : cases) {
    INFO(c.what);
    Rig rig;
    model::Instrument in = *rig.arena->project.instrument(kA);
    in.sample_rate = c.rate;
    in.sampler.play_mode = c.play_mode;
    in.sampler.volume = c.level;
    NEED(rig.arena->project.set_instrument(kA, in) == model::Status::Ok);
    rig.arena->project.set_volume_law(c.law);
    if (c.engine >= 0) {
      rig.bank.set_engine(kA, static_cast<std::uint8_t>(c.engine));
    }
    rig.select(kA);
    rig.clear_notes();
    rig.shift_tap(Button::F5);
    const std::string note = rig.note();
    rig.jobs(2000);
    const std::vector<std::string> files = pti_files(rig.card);
    if (c.refusal != nullptr) {
      CHECK(note == c.refusal);
      CHECK(files.empty());
      CHECK(rig.card.ops == 0u);  // nothing was created, not even the folder
      CHECK(rig.saves.pti_resampled_hz() == 0u);
      continue;
    }
    CHECK(rig.saves.pti_resampled_hz() == static_cast<std::uint32_t>(c.resampled_hz));
    NEED(files.size() == 1);
    model::Instrument back;
    std::uint32_t frames = 0;
    NEED(read_pti(*rig.card.get(files[0].c_str()), kA, back, frames));
    CHECK(back.sampler.volume == static_cast<std::uint8_t>(c.written_level));
    const app::OgPcmPlan plan = c.engine == audio::kEngineAmiga || (c.engine < 0 && c.rate == 8287u) ? app::og_pcm_plan_amiga(kFramesA)
                                                                                                       : app::og_pcm_plan(kFramesA, c.resampled_hz != 0 ? c.rate : 0u);
    CHECK(frames == plan.frames_out);
    CHECK(rig.card.get(files[0].c_str())->size() == formats::og::kInstrumentBytes + 2u * plan.frames_out);
    CHECK(rig.card.violations == 0u);
    for (const auto& kv : rig.card.files) {
      CHECK(kv.first.find("_o3save") == std::string::npos);
    }
  }
}

namespace {

// The frequency of the PCM of a .pti on the card in Hz at 44,100 Hz, by the zero crossings of the middle of the file (rising only).
double file_hz(const Bytes& file) {
  const std::size_t head = formats::og::kInstrumentBytes;
  const std::size_t frames = (file.size() - head) / 2u;
  auto at = [&](std::size_t i) { return static_cast<std::int16_t>(static_cast<std::uint16_t>(file[head + 2 * i] | (file[head + 2 * i + 1] << 8))); };
  const std::size_t from = frames / 4u;
  const std::size_t to = frames - frames / 4u;
  std::size_t first = 0, last = 0, n = 0;
  for (std::size_t i = from + 1; i < to; ++i) {
    if (at(i - 1) < 0 && at(i) >= 0) {
      if (n == 0u) {
        first = i;  // (the crossing's fractional part is below the tolerance of the checks that use this)
      }
      last = i;
      ++n;
    }
  }
  return n < 2u ? 0.0 : 44100.0 * static_cast<double>(n - 1u) / static_cast<double>(last - first);
}

}  // namespace

// The point of C1b: the file sounds at the pitch of the instrument. A tone in the bank, the instrument saved, the tone measured in the file, which an OG folder holds at 44,100 Hz.
TEST_CASE("save: the .pti of a 22,050 Hz sample plays at the pitch of the instrument, the file holds twice the frames, the points are the instrument's") {
  Rig rig;
  std::int16_t* p = const_cast<std::int16_t*>(rig.pcm(kA).data);
  for (std::uint32_t i = 0; i < kFramesA; ++i) {
    p[i] = static_cast<std::int16_t>(std::lround(12000.0 * std::sin(2.0 * 3.14159265358979323846 * 1000.0 * static_cast<double>(i) / 22050.0)));  // 1 kHz at the recorded rate
  }
  rig.select(kA);
  rig.clear_notes();
  rig.shift_tap(Button::F5);
  CHECK(rig.note() == "RESAMPLED 22050 -> 44100 HZ");
  rig.jobs(2000);
  const std::vector<std::string> files = pti_files(rig.card);
  NEED(files.size() == 1);
  const Bytes& file = *rig.card.get(files[0].c_str());
  CHECK(file.size() == formats::og::kInstrumentBytes + 2u * 2u * kFramesA);
  const double hz = file_hz(file);
  INFO("measured " << hz << " Hz");
  CHECK(hz > 995.0);
  CHECK(hz < 1005.0);
  model::Instrument back;
  std::uint32_t frames = 0;
  NEED(read_pti(file, kA, back, frames));
  CHECK(frames == 2u * kFramesA);
  CHECK(back.sampler.start == rig.inst(kA)->sampler.start);  // the points are fractions of the length: unchanged
  CHECK(back.sampler.end == rig.inst(kA)->sampler.end);
  // the bank's audio is untouched
  CHECK(rig.pcm(kA).frames == kFramesA);
  CHECK(rig.card.violations == 0u);
}

// The Amiga voice plays the data at 8,287.14 Hz at the unity note, whatever the label says, and adds the tune to the note: the file is the data resampled from 8,287.14 Hz with the tune kept and the fine tune the
// voice rounds to.
TEST_CASE("save: the .pti of an Amiga-engine instrument is the data at 8,287.14 Hz resampled, its tune kept and its fine tune the Amiga voice's MED step") {
  struct Case {
    std::uint32_t label;
    std::int8_t tune;
    std::int8_t fine;
    std::int8_t fine_written;
  };
  const Case cases[] = {{0, 0, 0, 0}, {44100, 0, 0, 0}, {8287, 0, 30, 25}, {16574, 12, 0, 0}, {8363, -12, -100, -100}, {0, 24, 7, 13}};
  for (const Case& c : cases) {
    INFO("label " << c.label << " tune " << int(c.tune) << " fine " << int(c.fine));
    Rig rig;
    rig.bank.set_engine(kA, audio::kEngineAmiga);
    model::Instrument in = *rig.arena->project.instrument(kA);
    in.sample_rate = c.label;
    in.sampler.play_mode = 0;
    in.sampler.tune = c.tune;
    in.sampler.fine_tune = c.fine;
    NEED(rig.arena->project.set_instrument(kA, in) == model::Status::Ok);
    std::int16_t* p = const_cast<std::int16_t*>(rig.pcm(kA).data);
    for (std::uint32_t i = 0; i < kFramesA; ++i) {
      p[i] = static_cast<std::int16_t>(std::lround(12000.0 * std::sin(2.0 * 3.14159265358979323846 * static_cast<double>(i) / 10.0)));  // 10 frames a cycle
    }
    rig.select(kA);
    rig.clear_notes();
    rig.shift_tap(Button::F5);
    CHECK(rig.note() == "RESAMPLED 8287 -> 44100 HZ");
    rig.jobs(3000);
    const std::vector<std::string> files = pti_files(rig.card);
    NEED(files.size() == 1);
    const Bytes& file = *rig.card.get(files[0].c_str());
    const app::OgPcmPlan plan = app::og_pcm_plan_amiga(kFramesA);
    CHECK(file.size() == formats::og::kInstrumentBytes + 2u * plan.frames_out);
    // 8,287.14 / 10 = 828.7 Hz in the data, 44,100 / 53.21 frames in the file: the same time per cycle
    const double hz = file_hz(file);
    INFO("measured " << hz << " Hz");
    CHECK(hz > 828.7 * 0.995);
    CHECK(hz < 828.7 * 1.005);
    model::Instrument back;
    std::uint32_t frames = 0;
    NEED(read_pti(file, kA, back, frames));
    CHECK(frames == plan.frames_out);
    CHECK(back.sampler.tune == c.tune);                  // the tune stays: an Amiga note n + tune plays the data at 8,287 x 2^((n + tune - 60) / 12), a Modern note the resampled data at the same pitch
    CHECK(back.sampler.fine_tune == c.fine_written);     // and the fine tune is the one the Amiga voice plays (a MED step, 12.5 hundredths)
    CHECK(rig.inst(kA)->sampler.fine_tune == c.fine);    // the instrument itself is not touched
  }
}

TEST_CASE("save: a resampled .pti follows its loop: the loop points are the instrument's fractions and the loop is seamless in the new rate") {
  Rig rig;
  model::Instrument in = *rig.arena->project.instrument(kA);
  in.sample_rate = 22050;
  in.sampler.play_mode = 1;  // forward loop
  in.sampler.start = 0;
  in.sampler.end = 65535;
  in.sampler.loop_start = 8192;
  in.sampler.loop_end = 57344;  // 1/8 .. 7/8 of 8000 frames: 1000 .. 7000, 6000 frames = 120 cycles of a 50-frame sine
  NEED(rig.arena->project.set_instrument(kA, in) == model::Status::Ok);
  std::int16_t* p = const_cast<std::int16_t*>(rig.pcm(kA).data);
  for (std::uint32_t i = 0; i < kFramesA; ++i) {
    p[i] = static_cast<std::int16_t>(std::lround(12000.0 * std::sin(2.0 * 3.14159265358979323846 * static_cast<double>(i) / 50.0)));
  }
  rig.select(kA);
  rig.clear_notes();
  rig.shift_tap(Button::F5);
  rig.jobs(2000);
  const std::vector<std::string> files = pti_files(rig.card);
  NEED(files.size() == 1);
  const Bytes& file = *rig.card.get(files[0].c_str());
  model::Instrument back;
  std::uint32_t frames = 0;
  NEED(read_pti(file, kA, back, frames));
  CHECK(frames == 16000u);
  CHECK(back.sampler.loop_start == 8192);
  CHECK(back.sampler.loop_end == 57344);
  CHECK(back.sampler.play_mode == 1);
  // the last frames before the loop's end (output frame 14000) continue the sine across the wrap: a frame of the new rate is a quarter of a source frame's 360/50 degrees; the first frames of the
  // loop (output frame 2000) are the sine's continuation, so the step across the wrap is a normal step, not a jump
  const std::size_t head = formats::og::kInstrumentBytes;
  auto at = [&](std::size_t i) { return static_cast<int>(static_cast<std::int16_t>(static_cast<std::uint16_t>(file[head + 2 * i] | (file[head + 2 * i + 1] << 8)))); };
  int biggest_step = 0;
  for (std::size_t i = 13900; i < 13999; ++i) {
    biggest_step = std::max(biggest_step, std::abs(at(i + 1) - at(i)));
  }
  const int wrap_step = std::abs(at(2000) - at(13999));  // the loop's last frame to its first
  INFO("biggest step " << biggest_step << ", across the wrap " << wrap_step);
  CHECK(wrap_step <= biggest_step + biggest_step / 4);
}

// The resampled audio is longer than a .pti holds: never cut, never written (OS-FILES-C section 3).
TEST_CASE("save: audio that resamples to more than a .pti holds is refused and nothing reaches the card") {
  Rig rig;
  rig.add_sample(kFree, "BIG", 150000, 3);  // 150,000 frames at 1,000 Hz: x44.1 = 6.6 M frames, over the 5,898,240 the sample memory holds
  rig.set_rate(kFree, 1000);
  rig.select(kFree);
  rig.clear_notes();
  rig.shift_tap(Button::F5);
  CHECK(rig.note() == "TOO LONG FOR A .PTI AT 44100 HZ");
  rig.jobs();
  CHECK(pti_files(rig.card).empty());
  CHECK(rig.card.ops == 0u);
}

// The sample changing under the save (an edit, a replace) ends it, as for a plain .pti: the resampler reads the bank's block in place.
TEST_CASE("save: a resampled .pti ends with CHANGED WHILE SAVING when the audio changes during the save, and leaves no file") {
  Rig rig;
  rig.select(kA);
  rig.clear_notes();
  rig.shift_tap(Button::F5);  // kA is 22,050 Hz in the rig: resampled, so the save takes many steps
  for (int i = 0; i < 6; ++i) {
    rig.pass(1);
  }
  NEED(rig.app->session().jobs.running() != 0u);
  std::uint32_t cap = 0;
  std::int16_t* q = rig.bank.reserve(cap);
  NEED(q != nullptr);
  fill(q, 2000, 9);
  NEED(rig.bank.commit_load(kA, q, 2000));
  rig.jobs(2000);
  CHECK(pti_files(rig.card).empty());
  for (const auto& kv : rig.card.files) {
    CHECK(kv.first.find("_o3save") == std::string::npos);
  }
}

// A project edit (not the audio) during the resampling ends the save the same way: the writer sees the project's generation move.
TEST_CASE("save: a resampled .pti ends with CHANGED WHILE SAVING when the project changes during the save, and leaves no file") {
  Rig rig;
  rig.select(kA);
  rig.clear_notes();
  rig.shift_tap(Button::F5);
  for (int i = 0; i < 6; ++i) {
    rig.pass(1);
  }
  NEED(rig.app->session().jobs.running() != 0u);
  model::Instrument in = *rig.arena->project.instrument(kA);
  in.sampler.volume = static_cast<std::uint8_t>(in.sampler.volume == 33u ? 34u : 33u);
  NEED(rig.arena->project.set_instrument(kA, in) == model::Status::Ok);
  rig.jobs(2000);
  CHECK(pti_files(rig.card).empty());
  for (const auto& kv : rig.card.files) {
    CHECK(kv.first.find("_o3save") == std::string::npos);
  }
}

// The strip says what the last save said: a refusal after a resampled save must not show the earlier RESAMPLED (the rate is cleared by every save_instrument).
TEST_CASE("save: a refused .pti save shows its refusal, not the RESAMPLED of the save before it") {
  Rig rig;
  rig.select(kA);
  rig.clear_notes();
  rig.shift_tap(Button::F5);
  CHECK(rig.note() == "RESAMPLED 22050 -> 44100 HZ");
  rig.jobs(2000);
  CHECK(pti_files(rig.card).size() == 1u);
  CHECK(rig.saves.pti_resampled_hz() == 22050u);
  rig.bank.set_engine(kA, audio::kEngineBits12);
  rig.clear_notes();
  rig.shift_tap(Button::F5);
  CHECK(rig.note() == "12/8 BIT ENGINE: NOT SAVED AS A .PTI");
  CHECK(rig.saves.pti_resampled_hz() == 0u);
  rig.jobs(2000);
  CHECK(pti_files(rig.card).size() == 1u);
}

// The resampler wraps at the SOURCE frames of the loop (floor(point x frames / 65535)); the file stores the points, which a player reads as floor(point x frames_out / 65535). With a fraction
// that is not exact the two differ by one output frame (known limit, docs/specs/OS-FILES-C.md section 3): the step across the loop's seam is then at most about two normal steps, never a jump.
TEST_CASE("save: a resampled .pti with loop points that do not fall on whole output frames seams within one output frame") {
  Rig rig;
  model::Instrument in = *rig.arena->project.instrument(kA);
  in.sample_rate = 22050;
  in.sampler.play_mode = 1;
  in.sampler.start = 0;
  in.sampler.end = 65535;
  in.sampler.loop_start = 10000;  // source frame 1220, output frame 2441 (2 x 1220 = 2440)
  in.sampler.loop_end = 55555;    // source frame 6781, output frame 13563 (2 x 6781 = 13562)
  NEED(rig.arena->project.set_instrument(kA, in) == model::Status::Ok);
  const app::OgLoop loop = app::og_loop_of(*rig.inst(kA), kFramesA);
  NEED(loop.active);
  NEED(loop.start == 1220u);
  NEED(loop.end == 6781u);
  NEED(app::og_point_frame(10000, 2u * kFramesA) == 2441u);
  NEED(app::og_point_frame(55555, 2u * kFramesA) == 13563u);
  std::int16_t* p = const_cast<std::int16_t*>(rig.pcm(kA).data);
  for (std::uint32_t i = 0; i < kFramesA; ++i) {
    p[i] = static_cast<std::int16_t>(std::lround(12000.0 * std::sin(2.0 * 3.14159265358979323846 * static_cast<double>(i) / 83.0)));  // 5561 = 67 x 83 frames in the loop
  }
  rig.select(kA);
  rig.clear_notes();
  rig.shift_tap(Button::F5);
  rig.jobs(2000);
  const std::vector<std::string> files = pti_files(rig.card);
  NEED(files.size() == 1);
  const Bytes& file = *rig.card.get(files[0].c_str());
  model::Instrument back;
  std::uint32_t frames = 0;
  NEED(read_pti(file, kA, back, frames));
  CHECK(frames == 2u * kFramesA);
  CHECK(back.sampler.loop_start == 10000);
  CHECK(back.sampler.loop_end == 55555);
  const std::size_t head = formats::og::kInstrumentBytes;
  auto at = [&](std::size_t i) { return static_cast<int>(static_cast<std::int16_t>(static_cast<std::uint16_t>(file[head + 2 * i] | (file[head + 2 * i + 1] << 8)))); };
  int biggest_step = 0;
  for (std::size_t i = 13400; i < 13562; ++i) {
    biggest_step = std::max(biggest_step, std::abs(at(i + 1) - at(i)));
  }
  const int wrap_step = std::abs(at(2441) - at(13562));  // the voice's last loop frame to its first
  INFO("biggest step " << biggest_step << ", across the wrap " << wrap_step);
  CHECK(wrap_step <= 2 * biggest_step + biggest_step / 4);
}

// The Amiga engine's file is the data through og_resample at the Paula step: the bytes are the resampler's own, frame for frame.
TEST_CASE("save: the .pti of an Amiga-engine instrument holds the bytes og_resample makes of the data") {
  Rig rig;
  rig.bank.set_engine(kA, audio::kEngineAmiga);
  model::Instrument in = *rig.arena->project.instrument(kA);
  in.sample_rate = 0;
  in.sampler.play_mode = 0;
  NEED(rig.arena->project.set_instrument(kA, in) == model::Status::Ok);
  rig.select(kA);
  rig.clear_notes();
  rig.shift_tap(Button::F5);
  rig.jobs(3000);
  const std::vector<std::string> files = pti_files(rig.card);
  NEED(files.size() == 1);
  const Bytes& file = *rig.card.get(files[0].c_str());
  const app::OgPcmPlan plan = app::og_pcm_plan_amiga(kFramesA);
  NEED(file.size() == formats::og::kInstrumentBytes + 2u * plan.frames_out);
  NEED(rig.bank.resample_kernel().ready());
  std::vector<std::int16_t> want(plan.frames_out);
  std::uint32_t clipped = 0;
  const app::OgLoop loop = app::og_loop_of(*rig.inst(kA), kFramesA);
  NEED(app::og_resample(rig.bank.resample_kernel(), rig.pcm(kA).data, kFramesA, plan.step_q32, plan.frames_out, loop, want.data(), 0, plan.frames_out, &clipped) == plan.frames_out);
  std::size_t differing = 0;
  for (std::uint32_t i = 0; i < plan.frames_out; ++i) {
    const std::uint16_t w = static_cast<std::uint16_t>(want[i]);
    const std::size_t o = formats::og::kInstrumentBytes + 2u * i;
    if (file[o] != (w & 0xFFu) || file[o + 1] != (w >> 8)) {
      ++differing;
    }
  }
  CHECK(differing == 0u);
}

// The strip's end of a resampled save says CLIPPED when the resampler saturated samples (a full-scale square rings past full scale at its edges), and plain DONE when it did not; a second press
// while the save runs is refused and leaves the notice for the save in progress.
TEST_CASE("save: a resampled .pti ends DONE (CLIPPED) for a full-scale square and plain DONE for a quiet source, a second press keeps the notice") {
  struct Case {
    std::int16_t high;
    std::int16_t low;
    bool second_press;
    bool clipped;
  };
  const Case cases[] = {{32767, -32768, false, true}, {8000, -8000, false, false}, {32767, -32768, true, true}, {8000, -8000, true, false}};
  for (const Case& c : cases) {
    INFO("high " << c.high << " second press " << c.second_press);
    Rig rig;
    std::int16_t* p = const_cast<std::int16_t*>(rig.pcm(kA).data);
    for (std::uint32_t i = 0; i < kFramesA; ++i) {
      p[i] = ((i / 20u) & 1u) != 0u ? c.high : c.low;
    }
    rig.select(kA);
    rig.clear_notes();
    rig.shift_tap(Button::F5);
    CHECK(rig.note() == "RESAMPLED 22050 -> 44100 HZ");
    if (c.second_press) {
      rig.pass(1);
      NEED(rig.app->session().jobs.running() != 0u);
      rig.shift_tap(Button::F5);
      CHECK(rig.note() == "A SAVE IS ALREADY RUNNING");
    }
    rig.jobs(2000);
    std::string end;  // the queued line that says the save ended (the head may still be an earlier one)
    for (unsigned i = 0; i < rig.app->state().notify.size(); ++i) {
      const std::string text = rig.app->state().notify.at(i).text.c_str();
      if (text.find("DONE") != std::string::npos) {
        end = text;
      }
    }
    INFO(end);
    CHECK(!end.empty());
    CHECK((end.find("(CLIPPED)") != std::string::npos) == c.clipped);
    CHECK(pti_files(rig.card).size() == 1u);
  }
}

TEST_CASE("pti: level_1114_to_12 keeps the gain within half a step of the 1.2+ law, and clamps under -24 dB") {
  bool clamped = false;
  CHECK(app::level_1114_to_12(0, clamped) == 0);
  CHECK_FALSE(clamped);
  for (unsigned l = 1; l <= 100; ++l) {
    const std::uint8_t n = app::level_1114_to_12(l, clamped);
    if (l < 40u) {
      CHECK(clamped);
      CHECK(n == 1);
      continue;
    }
    CHECK_FALSE(clamped);
    CHECK(n >= 1);
    CHECK(n <= 50);
    const double want_db = 0.4 * (static_cast<double>(l) - 100.0);
    const double got_db = n == 1 ? -24.0 : 0.48 * (static_cast<double>(n) - 50.0);
    const double err = got_db > want_db ? got_db - want_db : want_db - got_db;
    CHECK_MESSAGE(err <= (n <= 2 ? 0.481 : 0.241), "level "  /* the 1.2+ law has a 0.96 dB gap between its level 1 (-24.00) and 2 (-23.04) */ << l << " -> " << static_cast<unsigned>(n));
  }
}

TEST_CASE("load: the chooser's F5 lists instruments/*.pti and F1 loads the one under the cursor into the slot: one undo unit, the audio with it") {
  Rig rig;
  // A file the way SAVE makes it.
  const Bytes file = pti_bytes(rig.arena->project, kA, rig.pcm(kA).data, kFramesA);
  rig.card.put("instruments/kick.pti", file);
  rig.card.put("instruments/readme.txt", std::string("not an instrument"));
  rig.card.make_dir_now("instruments");
  press_load(rig, kFree);
  CHECK(rig.app->state().screen == app::ScreenId::SampleLoad);
  CHECK(rig.app->state().wav.pti);
  rig.jobs();
  rig.pass(100);
  const app::DirScan* scan = rig.app->model().files;
  NEED(scan != nullptr);
  NEED(scan->index().count() == 1u);  // the .pti, and not the text file
  CHECK(std::string(scan->index().name(0)) == "kick.pti");
  const std::size_t depth = rig.depth();
  const std::uint32_t free_before = rig.bank.free_samples();
  rig.tap(Button::F1);
  rig.jobs();
  rig.pass(100);
  NEED(rig.inst(kFree) != nullptr);
  CHECK(rig.app->state().screen == app::ScreenId::Instrument);
  CHECK(rig.app->state().instrument == static_cast<int>(kFree) + 1);
  CHECK(std::string(rig.inst(kFree)->name) == "KICK");
  CHECK(std::string(rig.inst(kFree)->sample_path) == "samples/instr12.wav");
  CHECK(rig.inst(kFree)->sampler.start == 1000);
  CHECK(rig.depth() == depth + 1);
  CHECK(rig.pcm(kFree).frames == kFramesA);
  CHECK(holds(rig.pcm(kFree).data, kFramesA, 1));
  CHECK(rig.pcm(kFree).data != rig.pcm(kA).data);          // read from the card: its own block
  CHECK(free_before - rig.bank.free_samples() >= kFramesA);  // ... which costs arena
  CHECK_FALSE(rig.app->state().wav.pti);                   // the sample browser is the sample browser again
  // Undo takes the instrument and the audio away; redo brings both back.
  rig.undo();
  CHECK(rig.inst(kFree) == nullptr);
  CHECK(rig.pcm(kFree).data == nullptr);
  rig.redo();
  NEED(rig.inst(kFree) != nullptr);
  CHECK(holds(rig.pcm(kFree).data, kFramesA, 1));
  rig.settle();
  CHECK_FALSE(rig.bank.rebind_pending());
}

TEST_CASE("load: a bad, short, stereo-length or empty .pti refuses cleanly: the strip says why, the slot, the bank and the undo history are untouched") {
  Rig rig;
  const Bytes good = pti_bytes(rig.arena->project, kA, rig.pcm(kA).data, kFramesA);
  struct Case {
    const char* name;
    Bytes bytes;
    const char* why;
  };
  std::vector<Case> cases;
  cases.push_back({"short.pti", Bytes(good.begin(), good.begin() + 100), "TRUNCATED"});
  cases.push_back({"wrong.pti", [&] { Bytes b = good; b[0] = 'X'; return b; }(), "UNKNOWN FILE TYPE"});
  cases.push_back({"half.pti", Bytes(good.begin(), good.end() - 3000), "UNSUPPORTED FORMAT"});
  cases.push_back({"silent.pti", Bytes(good.begin(), good.begin() + formats::og::kInstrumentBytes), "NO AUDIO IN THE FILE"});
  cases.push_back({"empty.pti", Bytes{}, "TRUNCATED"});
  for (const Case& c : cases) {
    INFO("file: " << c.name);
    Rig r;
    r.card.put((std::string("instruments/") + c.name).c_str(), c.bytes);
    r.card.make_dir_now("instruments");
    const std::uint32_t free_before = r.bank.free_samples();
    const std::size_t depth = r.depth();
    press_load(r, kFree);
    r.jobs();
    r.pass(100);
    NEED(r.app->model().files != nullptr);
    NEED(r.app->model().files->index().count() == 1u);
    r.tap(Button::F1);
    r.jobs();
    r.pass(100);
    CHECK(r.inst(kFree) == nullptr);
    CHECK(r.pcm(kFree).data == nullptr);
    CHECK(r.bank.free_samples() == free_before);
    CHECK(r.depth() == depth);
    CHECK_FALSE(r.bank.wav_open());
    CHECK(r.app->state().screen == app::ScreenId::SampleLoad);  // the screen stays: another file can be chosen
    std::string strip;
    for (int i = 0; i < 4 && strip.find(c.why) == std::string::npos; ++i) {
      strip = r.note();
      r.pass(1);
    }
    CHECK_MESSAGE(r.note().find(c.why) != std::string::npos, r.note());
  }
}

TEST_CASE("load: a .pti over a MIDI-type slot is refused, the strip says why, the slot and the undo history stay (OS-QAFIX-E-LOAD-25)") {
  Rig rig;
  rig.card.put("instruments/kick.pti", pti_bytes(rig.arena->project, kA, rig.pcm(kA).data, kFramesA));
  rig.card.make_dir_now("instruments");
  NEED(rig.inst(kMidi) != nullptr);
  NEED(rig.inst(kMidi)->kind == model::InstrumentKind::Midi);
  const std::size_t depth = rig.depth();
  const std::uint32_t free_before = rig.bank.free_samples();
  rig.select(kMidi);
  rig.clear_notes();
  (void)rig.invoke("instrument.load");  // the chooser's LOAD (a MIDI instrument has other keys under F5)
  CHECK(rig.app->state().screen == app::ScreenId::SampleLoad);
  rig.jobs();
  rig.pass(100);
  NEED(rig.app->model().files != nullptr);
  NEED(rig.app->model().files->index().count() == 1u);
  rig.tap(Button::F1);
  rig.jobs();
  rig.pass(100);
  CHECK(rig.note().find("SLOT IS MIDI") != std::string::npos);
  CHECK(rig.inst(kMidi)->kind == model::InstrumentKind::Midi);
  CHECK(rig.inst(kMidi)->midi_channel == 3);
  CHECK(rig.pcm(kMidi).data == nullptr);
  CHECK(rig.bank.free_samples() == free_before);
  CHECK(rig.depth() == depth);
  CHECK_FALSE(rig.bank.wav_open());
  CHECK(rig.app->state().screen == app::ScreenId::SampleLoad);
}

TEST_CASE("load: ADD is for sample files, a file with another name is not an instrument file, and a full arena says so") {
  Rig rig;
  const Bytes good = pti_bytes(rig.arena->project, kA, rig.pcm(kA).data, kFramesA);
  rig.card.put("instruments/kick.pti", good);
  rig.card.make_dir_now("instruments");
  press_load(rig, kFree);
  rig.jobs();
  rig.pass(100);
  rig.clear_notes();
  rig.tap(Button::F4);  // ADD
  CHECK(rig.note() == "ADD IS FOR SAMPLE FILES");
  CHECK(rig.inst(kFree) == nullptr);
  // The arena has no room for the audio.
  Rig tight;
  tight.card.put("instruments/kick.pti", good);
  tight.card.make_dir_now("instruments");
  std::uint32_t cap = 0;
  (void)tight.bank.reserve(cap);
  (void)tight.bank.alloc(cap - 4000u);  // 4000 samples left
  press_load(tight, kFree);
  tight.jobs();
  tight.pass(100);
  tight.tap(Button::F1);
  tight.jobs();
  tight.pass(100);
  CHECK(tight.inst(kFree) == nullptr);
  CHECK(tight.note().find("ARENA") != std::string::npos);
}

TEST_CASE("load: a module in the slot is in the way (the rule of PASTE): the list says so and nothing is read") {
  Rig rig;
  const Bytes good = pti_bytes(rig.arena->project, kA, rig.pcm(kA).data, kFramesA);
  rig.card.put("instruments/kick.pti", good);
  rig.card.make_dir_now("instruments");
  rig.select(kModule);
  rig.clear_notes();
  (void)rig.invoke("instrument.load");  // (the chooser's key is on a free slot; the action itself does not look at the slot)
  rig.jobs();
  rig.pass(100);
  NEED(rig.app->model().files != nullptr);
  NEED(rig.app->model().files->index().count() == 1u);
  rig.clear_notes();
  rig.tap(Button::F1);
  rig.jobs();
  CHECK(rig.note() == "SLOT HAS A MODULE: INSTRUMENT, SHIFT+F4 DELETE");
  CHECK(rig.inst(kModule)->kind == model::InstrumentKind::Module);
  CHECK_FALSE(rig.bank.wav_open());
}

TEST_CASE("load: a file that decodes to the instrument the slot already holds records no unit and spends no arena: nothing is swapped, and the undo of the earlier paste is still its own") {
  Rig rig;
  rig.copy(kA);
  rig.paste(kFree);  // the swap of this unit
  const std::int16_t* shared = rig.pcm(kFree).data;
  // The slot holds what the file decodes to (a .pti has no sample rate, so the paste is not byte-equal to its own file): one more unit, with no audio swap.
  const Bytes first = pti_bytes(rig.arena->project, kFree, rig.pcm(kFree).data, kFramesA);
  model::Instrument decoded;
  std::uint32_t decoded_frames = 0;
  NEED(read_pti(first, kFree, decoded, decoded_frames));
  NEED(rig.editor->set_instrument(kFree, decoded) == model::Status::Ok);
  const Bytes file = pti_bytes(rig.arena->project, kFree, rig.pcm(kFree).data, kFramesA);
  rig.card.put("instruments/same.pti", file);
  rig.card.make_dir_now("instruments");
  rig.select(kFree);
  rig.clear_notes();
  (void)rig.invoke("instrument.load");
  rig.jobs();
  rig.pass(100);
  const std::size_t depth = rig.depth();
  const std::uint32_t free_before = rig.bank.free_samples();
  rig.tap(Button::F1);
  rig.jobs();
  rig.pass(100);
  CHECK(rig.depth() == depth);
  CHECK(rig.bank.free_samples() == free_before);
  CHECK(rig.pcm(kFree).data == shared);
  CHECK_FALSE(rig.bank.wav_open());
  CHECK(rig.app->state().screen == app::ScreenId::Instrument);
  rig.undo();  // the instrument set above
  CHECK(rig.pcm(kFree).data == shared);
  rig.undo();  // the paste, with its own swap (a swap pushed by the no-op load would be taken here instead)
  CHECK(rig.inst(kFree) == nullptr);
  CHECK(rig.pcm(kFree).data == nullptr);
  rig.redo();
  rig.redo();
  CHECK(rig.pcm(kFree).data == shared);
}

TEST_CASE("load: leaving the instrument list puts the sample browser back at the card's root with the WAV filter") {
  Rig rig;
  rig.card.make_dir_now("instruments");
  press_load(rig, kFree);
  CHECK(std::string(rig.app->state().wav.folder) == "instruments");
  rig.tap(Button::Params);
  CHECK(rig.app->state().screen == app::ScreenId::Instrument);
  CHECK_FALSE(rig.app->state().wav.pti);
  CHECK(std::string(rig.app->state().wav.folder).empty());
}

// ---- the guard ----------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("guard: instruments/ holds .pti files that are never replaced, never removed and never given a backup") {
  namespace sv = hal::storage::save;
  using sv::Place;
  CHECK(sv::classify("instruments") == Place::Folder);
  CHECK(sv::classify("Instruments") == Place::Folder);
  CHECK(sv::classify("instruments/KICK.pti") == Place::Target);
  CHECK(sv::classify("INSTRUMENTS/kick 2.PTI") == Place::Target);
  CHECK(sv::classify("instruments/_o3save.tmp") == Place::Temp);
  for (const char* none : {"instruments/", "instruments/x.wav", "instruments/x.pti.bak", "instruments/.pti", "instruments/ .pti", "instruments/CON.pti", "instruments/x..pti", "instruments/sub/x.pti",
                           "instruments/x.pti.pti.bak", "instrument/x.pti", "instrumentsx/x.pti", "modules/instruments/x.pti", "Projects/a/instruments/x.pti", "instruments/_o3save.tmp.bak",
                           "instruments/../Projects/a/project.o3pj", "instruments/a/b/c"}) {
    INFO("path: " << none);
    CHECK(sv::classify(none) == Place::None);
  }
  CHECK(sv::may_make_dir("instruments"));
  CHECK_FALSE(sv::may_make_dir("instruments/sub"));
  CHECK(sv::may_create("instruments/_o3save.tmp"));
  CHECK_FALSE(sv::may_create("instruments/KICK.pti"));
  CHECK(sv::may_remove("instruments/_o3save.tmp"));
  CHECK_FALSE(sv::may_remove("instruments/KICK.pti"));  // a Target is never removed
  CHECK(sv::may_rename("instruments/_o3save.tmp", "instruments/KICK.pti"));
  CHECK_FALSE(sv::may_rename("instruments/KICK.pti", "instruments/KICK.pti.bak"));
  CHECK_FALSE(sv::may_rename("instruments/KICK.pti", "instruments/SNARE.pti"));
  CHECK_FALSE(sv::may_rename("instruments/_o3save.tmp", "Samples/KICK.wav"));
  CHECK(std::string(sv::kInstrumentsDir) == "instruments");
  // The longest path fits the capacity.
  const std::string longest = std::string("instruments/") + std::string(sv::kNameMax, 'x') + sv::kPtiSuffix;
  CHECK(longest.size() + 1 <= sv::kPathCap);
  CHECK(sv::classify(longest.c_str()) == Place::Target);
}

// OS-STEREO (e): a stereo instrument's .pti is its mid M, the very bytes of a mono sample's; the format is the OG's as ever and the strip says that S is not in it.
TEST_CASE("save: a stereo instrument's .pti holds M as a mono sample's does, and the strip says MID WRITTEN (OS-STEREO (e))") {
  for (unsigned rate : {44100u, 22050u}) {
    INFO("rate " << rate);
    Bytes mono_file;
    {
      Rig rig;
      rig.set_rate(kA, rate);
      rig.select(kA);
      rig.clear_notes();
      rig.shift_tap(Button::F5);
      CHECK(rig.note() != "STEREO: MID WRITTEN, SIDE NOT KEPT");
      rig.jobs(2000);
      const std::vector<std::string> files = pti_files(rig.card);
      NEED(files.size() == 1);
      mono_file = *rig.card.get(files[0].c_str());
    }
    Rig rig;
    rig.set_rate(kA, rate);
    {  // the same M, now with a side block behind it: a stereo instrument
      const std::uint32_t even = (kFramesA + 1u) & ~1u;
      std::uint32_t cap = 0;
      std::int16_t* p = rig.bank.reserve(cap);
      NEED(p != nullptr);
      std::memcpy(p, rig.pcm(kA).data, kFramesA * sizeof(std::int16_t));
      for (std::uint32_t i = 0; i < kFramesA; ++i) {
        p[even + i] = static_cast<std::int16_t>(-static_cast<int>(i % 7) - 1);
      }
      model::Instrument in = *rig.arena->project.instrument(kA);
      in.stereo = 1;
      NEED(rig.arena->project.set_instrument(kA, in) == model::Status::Ok);
      NEED(rig.bank.commit_load(kA, p, kFramesA, p + even));
      NEED(rig.bank.edit_stereo(kA));
    }
    rig.select(kA);
    rig.clear_notes();
    rig.shift_tap(Button::F5);
    CHECK(rig.note() == (rate == 44100u ? "STEREO: MID WRITTEN, SIDE NOT KEPT" : "RESAMPLED 22050 -> 44100 HZ, MID ONLY"));
    rig.jobs(2000);
    const std::vector<std::string> files = pti_files(rig.card);
    NEED(files.size() == 1);
    CHECK(*rig.card.get(files[0].c_str()) == mono_file);  // byte for byte
  }
}
