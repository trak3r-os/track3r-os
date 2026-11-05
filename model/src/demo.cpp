// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/model/demo.h>

#include <cstring>

namespace o3os::model::demo {
namespace {

constexpr const char* kNames[kInstruments] = {"KICK", "SNARE", "HAT", "BASS"};
constexpr std::uint8_t kRoot = 60;  // C-5: the sample's own pitch

Step note_step(unsigned note, unsigned instrument, std::uint8_t volume = kVolumeUnset) noexcept {
  Step s = kEmptyStep;
  s.note = static_cast<std::uint8_t>(note + 1u);  // the model's note byte is the MIDI note plus one
  s.instr = static_cast<std::uint8_t>(instrument);
  s.vol = volume;
  return s;
}

Step off_step() noexcept {
  Step s = kEmptyStep;
  s.note = kNoteOff;
  return s;
}

}  // namespace

Status add_instruments(Project& project, std::uint8_t volume) noexcept {
  for (unsigned i = 0; i < kInstruments; ++i) {
    Instrument in;
    init_instrument(in, InstrumentKind::Sample);
    std::memcpy(in.name, kNames[i], std::strlen(kNames[i]));  // init_instrument zeroed the field: terminated
    in.sampler.volume = volume;
    const Status st = project.set_instrument(i, in);
    if (st != Status::Ok) {
      return st;
    }
  }
  return Status::Ok;
}

bool pattern_is_empty(const Project& project, std::uint32_t id) noexcept {
  const Pattern* p = project.pattern(id);
  if (p == nullptr) {
    return true;
  }
  for (unsigned t = 0; t < kMaxTracks; ++t) {
    for (unsigned s = 0; s < kMaxPatternLength; ++s) {
      if (!is_empty(p->steps[t][s])) {
        return false;
      }
    }
  }
  return true;
}

Status fill_pattern(Project& project, std::uint32_t id, bool set_tempo) noexcept {
  if (!pattern_is_empty(project, id)) {
    return Status::Exists;
  }
  if (project.pattern(id) == nullptr) {
    const Status st = project.create_pattern(id, kPatternSteps);
    if (st != Status::Ok) {
      return st;
    }
  }
  Status result = Status::Ok;
  const auto put = [&](unsigned track, unsigned step, const Step& s) {
    const Status st = project.set_step(id, track, step, s);
    result = result == Status::Ok ? st : result;
  };
  for (unsigned bar = 0; bar < 2; ++bar) {
    const unsigned b = bar * 16u;
    for (unsigned beat = 0; beat < 4; ++beat) {
      put(0, b + beat * 4u, note_step(kRoot, 1));  // KICK on every beat
    }
    put(1, b + 4u, note_step(kRoot, 2));  // SNARE on 2 and 4
    put(1, b + 12u, note_step(kRoot, 2));
    for (unsigned k = 0; k < 8; ++k) {  // HAT every second step, accents on the off-beats
      put(2, b + k * 2u, note_step(kRoot, 3, (k & 1u) != 0u ? 100 : 60));
    }
    // BASS: C2 C2 Eb2 / Bb1 Bb1 F2, each note two steps long (the Off ends it)
    static constexpr struct {
      std::uint8_t at;
      std::uint8_t note;
    } kBass[] = {{0, 48}, {3, 48}, {6, 51}, {8, 46}, {11, 46}, {14, 53}};
    for (const auto& n : kBass) {
      put(3, b + n.at, note_step(n.note, 4));
      put(3, b + n.at + 2u, off_step());
    }
  }
  put(0, 10, note_step(kRoot, 1, 80));  // ghost kicks
  put(0, 26, note_step(kRoot, 1, 80));
  put(1, 30, note_step(kRoot, 2, 60));  // ghost snare
  if (result != Status::Ok) {
    return result;
  }
  // OS-BUG-TEMPOLOAD: Only set the tempo for the boot/new path, not for the load path.
  // The load path preserves the project's saved tempo; the boot/new path sets it to 120 BPM.
  if (set_tempo) {
    return project.set_tempo_x100(kTempoX100);
  }
  return Status::Ok;
}

}  // namespace o3os::model::demo
