// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/song_map.h>

namespace o3os::app {

std::uint32_t SongTrackMap::compute(const model::Pattern& pattern) noexcept {
  std::uint32_t map = 0;
  for (unsigned t = 0; t < kTracks; ++t) {
    const unsigned length = pattern.track_length[t] < model::kMaxPatternLength ? pattern.track_length[t] : model::kMaxPatternLength;
    Cell cell = Empty;
    for (unsigned s = 0; s < length && cell != Note; ++s) {
      const model::Step& step = pattern.steps[t][s];
      if (step.note != model::kNoteEmpty) {
        cell = Note;
      } else if (!model::is_empty(step)) {
        cell = Fx;
      }
    }
    map |= static_cast<std::uint32_t>(cell) << (2u * t);
  }
  return map;
}

std::uint32_t SongTrackMap::get(const model::Project& project, unsigned id) const noexcept {
  if (id < 1u || id > model::kMaxPatternId) {
    return 0;
  }
  if (&project != project_) {
    project_ = &project;  // another project: nothing cached is its
    for (Entry& e : entry_) {
      e = Entry{};
    }
  }
  const model::Pattern* pattern = project.pattern(id);
  if (pattern == nullptr) {
    return 0;
  }
  const std::uint32_t generation = project.pattern_generation(id);
  Entry& e = entry_[id];
  if (e.generation != generation || generation == 0u) {
    e.map = compute(*pattern);
    e.generation = generation;
    ++computed_;
  }
  return e.map;
}

void SongDensity::scan(const model::Pattern& pattern, std::uint64_t& density, std::uint64_t& loudness) noexcept {
  density = 0;
  loudness = 0;
  for (unsigned t = 0; t < kTracks; ++t) {
    const unsigned length = pattern.track_length[t] < model::kMaxPatternLength ? pattern.track_length[t] : model::kMaxPatternLength;
    unsigned notes = 0;
    unsigned volume_sum = 0;
    for (unsigned s = 0; s < length; ++s) {
      const model::Step& step = pattern.steps[t][s];
      if (step.note >= model::kNoteFirst && step.note <= model::kNoteLast) {
        ++notes;
        volume_sum += step.vol <= model::kVolumeMax ? step.vol : model::kVolumeMax;  // (unset: the loudest)
      }
    }
    density |= static_cast<std::uint64_t>(level(notes, length)) << (4u * t);
    loudness |= static_cast<std::uint64_t>(loud(volume_sum, notes)) << (4u * t);
  }
}

std::uint64_t SongDensity::compute(const model::Pattern& pattern) noexcept {
  std::uint64_t density = 0;
  std::uint64_t loudness = 0;
  scan(pattern, density, loudness);
  return density;
}

bool SongDensity::refresh(const model::Project& project, unsigned id) const noexcept {
  if (id < 1u || id > model::kMaxPatternId) {
    return false;
  }
  if (&project != project_) {
    project_ = &project;  // another project: nothing cached is its
    for (std::uint32_t& g : generation_) {
      g = 0;
    }
  }
  const model::Pattern* pattern = project.pattern(id);
  if (pattern == nullptr) {
    return false;
  }
  const std::uint32_t generation = project.pattern_generation(id);
  if (generation_[id] != generation || generation == 0u) {
    scan(*pattern, dens_[id], loud_[id]);
    generation_[id] = generation;
    ++computed_;
  }
  return true;
}

std::uint64_t SongDensity::get(const model::Project& project, unsigned id) const noexcept { return refresh(project, id) ? dens_[id] : 0u; }

std::uint64_t SongDensity::loudness(const model::Project& project, unsigned id) const noexcept { return refresh(project, id) ? loud_[id] : 0u; }

unsigned SongUseCount::get(const model::Project& project, unsigned id) const noexcept {
  if (id < 1u || id > model::kMaxPatternId) {
    return 0;
  }
  if (!valid_ || &project != project_ || project.song_generation() != generation_) {
    for (std::uint8_t& c : count_) {
      c = 0;
    }
    const std::uint32_t length = project.song_length();  // (at most 255: a count never outgrows its byte)
    for (std::uint32_t pos = 0; pos < length; ++pos) {
      ++count_[project.order(pos)];
    }
    project_ = &project;
    generation_ = project.song_generation();
    valid_ = true;
    ++built_;
  }
  return count_[id];
}

}  // namespace o3os::app
