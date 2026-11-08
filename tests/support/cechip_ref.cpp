// SPDX-License-Identifier: GPL-3.0-or-later
//
// See cechip_ref.h. Compiled as C++20 inside the oracle library (third_party/tracker-ce-chip/CMakeLists.txt) because the shim's headers are.
#include "cechip_ref.h"

#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "ChiptuneSynthVoice.h"
#include "PolySynth/sdk/SynthVoiceSDK.h"

namespace cechip_ref {

namespace {

using polysynth::InstrumentType;
using polysynth::InstrumentVoice;
using polysynth::InstrumentVoiceContext;
using polysynth::Patch;

// The original's metadata, built once: createMetadata also sizes the static handler table the setters are called through.
const Patch::Metadata& metadata() {
  static const Patch::Metadata md = [] {
    const std::unique_ptr<InstrumentType> type = polysynth::createChiptuneSynth();
    Patch::Metadata m;
    type->createMetadata(Patch{}, m);
    return m;
  }();
  return md;
}

const Patch::Metadata::Param& param(int id) { return metadata().params.at(static_cast<std::size_t>(id)); }

}  // namespace

float sample_rate() { return polysynth::sdk::kSampleRate; }

int param_count() { return static_cast<int>(metadata().params.size()); }
const char* param_name(int id) { return param(id).displayName; }
float param_default(int id) { return param(id).defaultValue; }
float param_min(int id) { return param(id).minValue; }
float param_max(int id) { return param(id).maxValue; }

struct Voice::Impl {
  std::unique_ptr<InstrumentType> type;
  std::unique_ptr<InstrumentVoice> voice;
  InstrumentVoiceContext ctx;
};

Voice::Voice() : impl_(new Impl) {
  (void)metadata();  // fills the handler table before the first setter
  impl_->type = polysynth::createChiptuneSynth();
  impl_->voice = impl_->type->createVoice(0);
}

Voice::~Voice() { delete impl_; }

void Voice::set(int id, float ce_value) { impl_->voice->setParam(id, ce_value); }

void Voice::note_on(int note, float velocity, bool legato) { impl_->voice->onNoteOn(impl_->ctx, note, velocity, legato); }

void Voice::note_off(int note) { impl_->voice->onNoteOff(impl_->ctx, note); }

bool Voice::active() { return impl_->voice->isActive(); }

void Voice::render(float* out, std::size_t n) {
  polysynth::AudioBufferStereoRef output{std::span<float>(out, n), std::span<float>()};
  impl_->voice->renderVoiceAudio(impl_->ctx, std::nullopt, output, std::span<polysynth::AudioBufferRef>());
}

}  // namespace cechip_ref
