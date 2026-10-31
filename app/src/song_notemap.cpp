// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/song_notemap.h>

#include <o3os/app/pattern_view.h>

#include <cstring>

namespace o3os::app {

static_assert(SongNoteMap::kTracks == static_cast<unsigned>(PatternView::kMaxVisibleTracks), "the map covers the tracks Pattern can show");
static_assert(SongNoteMap::kTracks <= model::kMaxTracks, "the map's tracks are the model's");

void SongNoteMap::render(const model::Pattern& pattern, std::uint8_t (&out)[kBytes]) noexcept {
  std::memset(out, 0, kBytes);
  for (unsigned t = 0; t < kTracks; ++t) {
    const unsigned length = pattern.track_length[t] < kSteps ? pattern.track_length[t] : kSteps;
    std::uint8_t* row = out + 2u * t * kStride;
    for (unsigned s = 0; s < length; ++s) {
      const std::uint8_t note = pattern.steps[t][s].note;
      if (note >= model::kNoteFirst && note <= model::kNoteLast) {
        row[s / 8u] = static_cast<std::uint8_t>(row[s / 8u] | (0x80u >> (s % 8u)));
      }
    }
  }
}

bool SongNoteMap::ensure(hal::Display& display, const model::Project& project, unsigned id) const noexcept {
  if (id < 1u || id > model::kMaxPatternId) {
    return false;
  }
  const model::Pattern* pattern = project.pattern(id);
  if (pattern == nullptr) {
    return false;
  }
  if (&project != project_ || &display != display_) {  // another project, or another display: nothing recorded is in its RAM_G
    for (std::uint32_t& g : generation_) {
      g = 0;
    }
    project_ = &project;
    display_ = &display;
  }
  const std::uint32_t generation = project.pattern_generation(id);
  if (generation_[id] == generation && generation != 0u) {
    return true;
  }
  if (stopped_ || uploads_ >= kMaxPerFrame) {  // this frame's share is spent (or the display said Busy): the picture comes on a later frame
    pending_ = true;
    return false;
  }
  alignas(4) std::uint8_t bits[kBytes];
  static_assert(kBytes % hal::kUploadAlign == 0, "a picture is whole words");
  render(*pattern, bits);
  if (display.upload(addr(id), bits, kBytes) != hal::UploadResult::Ok) {
    stopped_ = true;
    pending_ = true;
    return false;
  }
  ++uploads_;
  generation_[id] = generation;
  ++built_;
  return true;
}

}  // namespace o3os::app
