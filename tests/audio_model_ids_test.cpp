// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-005b review: the audio library does not depend on the model (the layers: no header of the engine includes it, and platform code, which may not
// include the model, uses the ids through audio::). The graph's track count and the parameter ids it understands are therefore the audio library's own
// constants; this test is where they meet the model's and are held equal, at compile time, so that neither side can move without the other. Host
// only (the model library is not in the audio target suites); the static_asserts are the test, the cases check that the model really has what the
// ids point at and refuses the engine's own fields.
#include <doctest/doctest.h>

#include <o3os/audio/graph.h>
#include <o3os/audio/graph_params.h>
#include <o3os/audio/voices.h>
#include <o3os/core/param.h>
#include <o3os/model/limits.h>
#include <o3os/model/params.h>
#include <o3os/model/project.h>
#include <o3os/seq/fx.h>

#include <initializer_list>

namespace {

namespace gp = o3os::audio::graph_param;
namespace tp = o3os::model::track_param;
namespace mp = o3os::model::master_param;

static_assert(o3os::audio::kTracks == o3os::model::kMaxTracks, "the graph has the model's track slots");
static_assert(o3os::audio::kInputStrip == o3os::model::kMaxTracks, "the input strip follows the model's slots");
static_assert(o3os::audio::kVoiceTracks == o3os::model::kMaxTracks, "the voices serve the model's track slots");
static_assert(o3os::audio::kSampleSlots == o3os::model::kMaxInstruments, "a sample slot per instrument slot (a NoteOn names the slot, 0..63)");
static_assert(o3os::audio::kMaxSlices == o3os::model::kSlices, "a beat slice table holds the instrument's slices (OS-007h)");
static_assert(static_cast<unsigned>(o3os::seq::FxGroup::Slice) == o3os::audio::kModGroupSlice, "ModSet / ModEnd carry the sequencer's FX group, the voices read the Slice one (OS-007k)");
static_assert(static_cast<unsigned>(o3os::seq::FxGroup::Engine0) == o3os::audio::kModGroupEngine0 && static_cast<unsigned>(o3os::seq::FxGroup::Engine1) == o3os::audio::kModGroupEngine1,
              "the Amiga engine reads the sequencer's two engine FX groups (OS-008c)");
static_assert(o3os::seq::fx::kEngineFirst == o3os::audio::kFxEngineFirst, "the engine FX window starts where the sequencer says (OS-008c)");
static_assert(static_cast<unsigned>(o3os::seq::FxGroup::LfoFilter) == o3os::audio::kModGroupLfoFilter, "the voices read the sequencer's Filter LFO group (OS-FU2)");
static_assert(static_cast<unsigned>(o3os::seq::FxGroup::LfoVolume) == o3os::audio::kModGroupLfoVolume && static_cast<unsigned>(o3os::seq::FxGroup::LfoPanning) == o3os::audio::kModGroupLfoPanning &&
                  static_cast<unsigned>(o3os::seq::FxGroup::LfoFinetune) == o3os::audio::kModGroupLfoFinetune && static_cast<unsigned>(o3os::seq::FxGroup::LfoPosition) == o3os::audio::kModGroupLfoPosition &&
                  o3os::audio::kModGroupLfoVolume + 3u == o3os::audio::kModGroupLfoFilter && o3os::audio::kModGroupLfoPosition == o3os::audio::kModGroupLfoFilter + 1u,
              "the voices read the sequencer's LFO groups for the performance layer's VOL, PAN, TUNE, FLT and POS LFO (OS-PERF-LFO)");
static_assert(static_cast<unsigned>(o3os::seq::FxGroup::Slide) == o3os::audio::kModGroupSlide, "the voices read the sequencer's Slide group (OS-PFX)");
static_assert(o3os::seq::fx::kSlideUp == o3os::audio::kFxSlideUp && o3os::seq::fx::kSlideDown == o3os::audio::kFxSlideDown, "the voices read the sequencer's Slide FX numbers (OS-PFX)");
static_assert(static_cast<unsigned>(o3os::seq::FxGroup::Glide) == o3os::audio::kModGroupGlide && static_cast<unsigned>(o3os::seq::FxGroup::Overdrive) == o3os::audio::kModGroupOverdrive &&
                  static_cast<unsigned>(o3os::seq::FxGroup::BitDepth) == o3os::audio::kModGroupBitDepth,
              "the voices read the sequencer's Glide, Overdrive and Bit depth groups (OS-IFX1)");
static_assert(o3os::seq::fx::kGlide == o3os::audio::kFxGlide && o3os::seq::fx::kOverdrive == o3os::audio::kFxOverdrive && o3os::seq::fx::kBitDepth == o3os::audio::kFxBitDepth,
              "the voices read the sequencer's Glide, Overdrive and Bit depth FX numbers (OS-IFX1)");
static_assert(o3os::seq::fx::kTrkArp == o3os::audio::kFxTrkArp && o3os::seq::fx::kTrkVolSlide == o3os::audio::kFxTrkVolSlide && o3os::seq::fx::kTrkFineVol == o3os::audio::kFxTrkFineVol &&
                  o3os::seq::fx::kTrkFinePortaUp == o3os::audio::kFxTrkFinePortaUp && o3os::seq::fx::kTrkFinePortaDown == o3os::audio::kFxTrkFinePortaDown &&
                  o3os::seq::fx::kTrkArp == o3os::audio::kFxTrkFirst && o3os::seq::fx::kCoreLast == o3os::audio::kFxTrkLast,
              "the tracker FX engine reads the sequencer's FX numbers 46..50 (OS-COREFX, ADR 0004 amendment 2)");
static_assert(o3os::seq::fx::kTrkPanSlide == o3os::audio::kFxTrkPanSlide && o3os::seq::fx::kTrkVibWave == o3os::audio::kFxTrkVibWave && o3os::seq::fx::kTrkTremWave == o3os::audio::kFxTrkTremWave &&
                  o3os::seq::fx::kTrkGliss == o3os::audio::kFxTrkGliss && o3os::seq::fx::kTrkRetrig == o3os::audio::kFxTrkRetrig && o3os::seq::fx::kTrkTremor == o3os::audio::kFxTrkTremor &&
                  o3os::seq::fx::kTrkChanPan == o3os::audio::kFxTrkChanPan,
              "the tracker FX engine reads the sequencer's FX numbers 51..57 (OS-COREFX CF2, ADR 0004 amendment 2)");
static_assert(o3os::seq::fx::kTrkChanVol == o3os::audio::kFxTrkChanVol && o3os::seq::fx::kTrkChanVolSlide == o3os::audio::kFxTrkChanVolSlide && o3os::seq::fx::kTrkGlobalVol == o3os::audio::kFxTrkGlobalVol &&
                  o3os::seq::fx::kTrkGlobalVolSlide == o3os::audio::kFxTrkGlobalVolSlide && o3os::seq::fx::kTrkPanbrello == o3os::audio::kFxTrkPanbrello,
              "the tracker FX engine reads the sequencer's FX numbers 58..62 (OS-COREFX CF3, ADR 0004 amendment 2)");
static_assert(static_cast<unsigned>(o3os::seq::FxGroup::Panning) == o3os::audio::kModGroupPanning && o3os::seq::fx::kPanning == o3os::audio::kFxPanning,
              "the tracker FX engine reads the sequencer's Panning group and FX number (OS-COREFX CF2)");
static_assert(static_cast<unsigned>(o3os::seq::FxGroup::Volume) == o3os::audio::kModGroupVolume, "the strips read the sequencer's Volume group for the performance layer's VOLUME (OS-PERFFX P2)");
static_assert(static_cast<unsigned>(o3os::seq::FxGroup::DelaySend) == o3os::audio::kModGroupDelaySend && static_cast<unsigned>(o3os::seq::FxGroup::ReverbSend) == o3os::audio::kModGroupReverbSend &&
                  o3os::seq::fx::kDelaySend == o3os::audio::kFxDelaySend && o3os::seq::fx::kReverbSend == o3os::audio::kFxReverbSend,
              "the strips read the sequencer's Delay send and Reverb send groups and FX numbers (OS-PERFFX P3)");
static_assert(static_cast<unsigned>(o3os::seq::FxGroup::Tune) == o3os::audio::kModGroupTune && o3os::audio::kModGroupTune == 21u,
              "the voices read the sequencer's Tune group, the Perform screen's TUNE (OS-PERFFX P5); the groups appended to the enum keep every number before them");
static_assert(static_cast<unsigned>(o3os::seq::FxGroup::Position) == o3os::audio::kModGroupPosition && o3os::audio::kModGroupPosition == 9u &&
                  static_cast<unsigned>(o3os::seq::FxGroup::SampleEnd) == o3os::audio::kModGroupSampleEnd && o3os::audio::kModGroupSampleEnd == 22u && o3os::seq::kFxGroupCount == 23u,
              "the voices read the sequencer's Position and Sample end groups, the Perform screen's SMP POS and SMP END (OS-PERFFX P7)");
static_assert(static_cast<unsigned>(o3os::seq::FxGroup::Reverse) == o3os::audio::kModGroupReverse && o3os::audio::kModGroupReverse == 8u && o3os::seq::fx::kReverse == o3os::audio::kFxReverse,
              "the voices read the sequencer's Reverse group and FX number, the Perform screen's PLAYBACK and the step FX 26 (OS-PERF-REVERSE)");
static_assert(o3os::seq::fx::kFilterLfo == o3os::audio::kFxFilterLfo, "the voices read the sequencer's Filter LFO FX number (OS-FU5)");
static_assert(o3os::seq::fx::kVolumeLfo == o3os::audio::kFxLfoVolume && o3os::audio::kFxLfoVolume == 23 && o3os::seq::fx::kPanningLfo == o3os::audio::kFxLfoPanning && o3os::audio::kFxLfoPanning == 24 &&
                  o3os::seq::fx::kFinetuneLfo == o3os::audio::kFxLfoFinetune && o3os::audio::kFxLfoFinetune == 33 && o3os::seq::fx::kPositionLfo == o3os::audio::kFxLfoPosition &&
                  o3os::audio::kFxLfoPosition == 36,
              "the voices read the sequencer's Volume, Panning, Finetune and Position LFO FX numbers (OS-PERF-LFO-STEP)");
static_assert(static_cast<unsigned>(o3os::seq::FxGroup::Filter) == o3os::audio::kModGroupFilter, "the voices read the sequencer's Filter group (OS-007l)");
static_assert(o3os::seq::fx::kLowPass == o3os::audio::kFxFilterFirst && o3os::seq::fx::kHighPass == o3os::audio::kFxFilterFirst + 1 &&
                  o3os::seq::fx::kBandPass == o3os::audio::kFxFilterFirst + 2 && o3os::audio::kFxFilterCount == 3,
              "the Cutoff FX types are the sequencer's 27, 28, 29, in the order of the filter types low, high, band (OS-007l)");
static_assert(o3os::audio::kDefaultActiveMask == (1u << o3os::model::kDefaultActiveTracks) - 1u, "twelve tracks are active in a new project");
static_assert(gp::strip::Volume == tp::Volume && gp::strip::Pan == tp::Pan && gp::strip::Mute == tp::Mute && gp::strip::Solo == tp::Solo &&
                  gp::strip::Bus == tp::Bus,
              "the strip fields are the model's track parameters");
static_assert(gp::master::Volume == mp::Volume && gp::master::Dry == mp::Dry && gp::master::Reverb == mp::Reverb && gp::master::Delay == mp::Delay &&
                  gp::master::DryMute == mp::DryMute && gp::master::ReverbMute == mp::ReverbMute && gp::master::DelayMute == mp::DelayMute &&
                  gp::master::Limiter == mp::Limiter,
              "the master fields are the model's master parameters");
static_assert(gp::strip::Route >= tp::kCount && gp::strip::SendA >= tp::kCount && gp::strip::SendB >= tp::kCount && gp::strip::Source >= tp::kCount && gp::strip::XfGain >= tp::kCount,
              "an engine strip field does not collide with the model's");
static_assert(gp::master::CueLevel >= mp::kCount && gp::master::OutMode >= mp::kCount && gp::master::RetRouteA >= mp::kCount &&
                  gp::master::RetRouteB >= mp::kCount,
              "an engine master field does not collide with the model's");
static_assert(gp::master::DelayTime >= mp::kCount && gp::master::ReverbQuality >= mp::kCount && gp::master::DelayTime == o3os::audio::sendfx::kFieldBase[0] + o3os::audio::sendfx::delay::Time &&
                  gp::master::DelayMode == o3os::audio::sendfx::kFieldBase[0] + o3os::audio::sendfx::delay::Mode &&
                  gp::master::ReverbSize == o3os::audio::sendfx::kFieldBase[1] + o3os::audio::sendfx::reverb::Size &&
                  gp::master::ReverbQuality == o3os::audio::sendfx::kFieldBase[1] + o3os::audio::sendfx::reverb::Quality &&
                  gp::master::DelayClear == o3os::audio::sendfx::kFieldBase[0] + o3os::audio::sendfx::kClearField &&
                  gp::master::ReverbClear == o3os::audio::sendfx::kFieldBase[1] + o3os::audio::sendfx::kClearField && gp::master::VoiceSendsReset >= o3os::audio::sendfx::kFieldBase[1] + o3os::audio::sendfx::kFieldSpan,
              "the send effects' master fields (OS-SENDFX) are the engine's, past the model's, at their effect's base");
static_assert(gp::project::SendsMode == o3os::model::project_setting::SendsMode, "the graph's SENDS setting is the model's project setting (OS-SENDFX)");
namespace ia = o3os::audio::inst_param;
namespace im = o3os::model::instrument_param;
static_assert(ia::Volume == im::Volume && ia::Panning == im::Panning && ia::Tune == im::Tune && ia::FineTune == im::FineTune && ia::Glide == im::Glide &&
                  ia::FilterEnable == im::FilterEnable && ia::FilterType == im::FilterType && ia::Cutoff == im::Cutoff && ia::Resonance == im::Resonance &&
                  ia::DelaySend == im::DelaySend && ia::ReverbSend == im::ReverbSend && ia::Overdrive == im::Overdrive && ia::BitDepth == im::BitDepth && ia::PlayEngine == im::PlayEngine && ia::kBase == im::kBase,
              "the instrument fields the voices' descriptors take (OS-LIVEP) are the model's");
static_assert(ia::kEnvelopeFirst == im::kEnvelopeFirst && ia::kEnvelopeStride == im::kEnvelopeStride && ia::EnvEnable == im::EnvEnable && ia::EnvAmount == im::EnvAmount &&
                  ia::EnvAttack == im::EnvAttack && ia::EnvDecay == im::EnvDecay && ia::EnvSustain == im::EnvSustain && ia::EnvRelease == im::EnvRelease &&
                  ia::kEnvelopeFields == im::kEnvelopeFields && ia::kEnvelopes == im::kEnvelopes,
              "the envelope fields the voices' descriptors take (OS-LIVEP) are the model's");
static_assert(ia::kLfoFirst == im::kModFirst && ia::kLfoStride == im::kModStride && ia::LfoSource == im::ModSource && ia::LfoShape == im::LfoShape && ia::LfoSpeed == im::LfoSpeed &&
                  ia::LfoAmount == im::LfoAmount && ia::kLfoFields == im::kModFields && ia::lfo(5, 3) == im::mod(5, 3),
              "the SOURCE and LFO fields the voices take live (OS-LFOLIVE) are the model's");
static_assert(o3os::audio::kLfoShapeSine == o3os::model::kLfoShapeSine && o3os::audio::kLfoShapeCount == o3os::model::kLfoShapeCount && o3os::model::kLfoShapeCount == o3os::model::kLfoShapeSine + 1u,
              "the LFO shapes (OS-LFOSINE: 0..5, Sine last) the voices take are the model's");
static_assert(o3os::model::kMaxOutputBus == 16, "the Bus field's range (16) is the model's kMaxOutputBus");

}  // namespace

TEST_CASE("audio ids: the model has every parameter the graph takes from it, and refuses the engine's own fields") {
  using o3os::core::ParamKind;
  o3os::model::ParamField f;
  for (unsigned field = 0; field < tp::kCount; ++field) {
    CHECK(o3os::model::find_param(o3os::audio::strip_param(0, field), f));
    CHECK(o3os::model::find_param(o3os::audio::strip_param(15, field), f));
  }
  for (unsigned field = 0; field < mp::kCount; ++field) {
    CHECK(o3os::model::find_param(o3os::audio::master_param(field), f));
  }
  for (const unsigned field : {gp::strip::Route, gp::strip::SendA, gp::strip::SendB, gp::strip::Source, gp::strip::XfGain}) {
    CHECK_FALSE(o3os::model::find_param(o3os::audio::strip_param(0, field), f));
  }
  for (const unsigned field : {gp::master::CueLevel, gp::master::OutMode, gp::master::RetRouteA, gp::master::RetRouteB, gp::master::DelayTime, gp::master::DelayFeedback, gp::master::DelaySync,
                               gp::master::DelayRate, gp::master::DelayPingPong, gp::master::DelayTone, gp::master::DelayMode, gp::master::ReverbSize, gp::master::ReverbDamp,
                               gp::master::ReverbPredelay, gp::master::ReverbDiffusion, gp::master::ReverbQuality, gp::master::DelayClear, gp::master::ReverbClear, gp::master::VoiceSendsReset}) {
    CHECK_FALSE(o3os::model::find_param(o3os::audio::master_param(field), f));
  }
  CHECK_FALSE(o3os::model::find_param(o3os::audio::strip_param(o3os::audio::kInputStrip, tp::Volume), f));  // the input strip is the engine's, not the project's
  // the ranges the engine accepts are the model's: a volume or pan of 0..100, a switch of 0 or 1
  const o3os::core::ParamDesc* volume = o3os::model::param_desc(o3os::audio::strip_param(0, tp::Volume));
  const o3os::core::ParamDesc* pan = o3os::model::param_desc(o3os::audio::strip_param(0, tp::Pan));
  const o3os::core::ParamDesc* mute = o3os::model::param_desc(o3os::audio::strip_param(0, tp::Mute));
  const o3os::core::ParamDesc* bus = o3os::model::param_desc(o3os::audio::strip_param(0, tp::Bus));
  CHECK_FALSE(volume == nullptr);
  if (volume == nullptr || pan == nullptr || mute == nullptr || bus == nullptr) {
    return;
  }
  CHECK(volume->min == 0.0f);
  CHECK(volume->max == 100.0f);
  CHECK(pan->min == 0.0f);
  CHECK(pan->max == 100.0f);
  CHECK(mute->max == 1.0f);
  CHECK(bus->max == static_cast<float>(o3os::model::kMaxOutputBus));
}
