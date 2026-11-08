#include "config.h"
#ifdef FEATURE_SYNTH

#include "ChiptuneSynthVoice.h"
#include "PolySynth/sdk/SynthVoiceSDK.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace polysynth
{

namespace
{

enum class ChipParamId : int
{
    kModel = 0,
    kTune,
    kFine,
    kShape,
    kPwmRate,
    kPwmDepth,
    kSubMix,
    kArpChord,
    kArpSpeed,
    kPitchSweep,
    kSweepTime,
    kVibRate,
    kVibDepth,
    kAmpAttack,
    kAmpDecay,
    kAmpSustain,
    kAmpRelease,
    kFilterCutoff,
    kFilterResonance,
    kFilterEnvAmount,
    kBitcrush,
    kDecimate,
    kPlayMode,
    kGlideTime,
    kVolume,
};

enum class ChipModel : int
{
    kPulse2A03 = 0,
    kSidSawTri = 1,
    kNoiseLfsr = 2,
    kFmBell = 3,
    kSuperSquare = 4,
    kZapLaser = 5,
    kArpChord = 6,
    kChipDrum = 7,
};

} // namespace

namespace
{

static constexpr std::array<const char*, 8> kModelNames = {
    "2A03 Pulse",   "SID Saw/Tri", "Noise / LFSR", "FM Bell",
    "Super Square", "Zap / Laser", "Arp Chord",    "Chip Drum",
};

static constexpr std::array<const char*, 8> kArpNames = {
    "Off", "Octave", "Fifth", "Major", "Minor", "Sus4", "Maj7", "Min7",
};

// Semitone offsets for 4-step chiptune chord arpeggiator
static constexpr int8_t kArpChords[8][4] = {
    { 0, 0, 0, 0 },   // Off
    { 0, 12, 0, 12 }, // Octave
    { 0, 7, 12, 7 },  // Fifth
    { 0, 4, 7, 12 },  // Major
    { 0, 3, 7, 12 },  // Minor
    { 0, 5, 7, 12 },  // Sus4
    { 0, 4, 7, 11 },  // Maj7
    { 0, 3, 7, 10 },  // Min7
};

inline float polyBlep(float t, float dt)
{
    if (dt <= 0.0f)
    {
        return 0.0f;
    }
    if (t < dt)
    {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt)
    {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

inline float fastSine(float phase01)
{
    return std::sin(phase01 * 2.0f * std::numbers::pi_v<float>);
}

inline float nesSteppedTriangle(float phase01)
{
    const float tri = (phase01 < 0.5f) ? (phase01 * 4.0f - 1.0f) : (3.0f - phase01 * 4.0f);
    // 4-bit quantized 16-step triangle like the NES 2A03
    return std::round(tri * 7.5f) / 7.5f;
}

inline uint16_t stepLfsr(uint16_t lfsr, bool metallicShortMode)
{
    const uint16_t tap = metallicShortMode ? ((lfsr >> 6) & 1u) : ((lfsr >> 1) & 1u);
    const uint16_t bit = ((lfsr >> 0) ^ tap) & 1u;
    return static_cast<uint16_t>((lfsr >> 1) | (bit << 14));
}

class ChiptuneSynthVoice : public sdk::CustomSynthVoice<ChiptuneSynthVoice, ChipParamId>
{
  public:
    ChiptuneSynthVoice() = default;

    bool isActive() final { return envStage != EnvStage::kIdle; }

    void onNoteOn(const InstrumentVoiceContext& /*ctx*/,
                  note_t note,
                  float /*velocity*/,
                  bool isLegatoStep) final
    {
        const float targetNote = static_cast<float>(note);
        if (!hasPlayedAnyNote || glideTimeSec <= 0.001f ||
            (playMode == PlayMode::kLegato && !isLegatoStep))
        {
            currentGlideNote = targetNote;
        }
        targetGlideNote = targetNote;
        hasPlayedAnyNote = true;

        if (!isLegatoStep || envStage == EnvStage::kIdle || envStage == EnvStage::kRelease)
        {
            envStage = EnvStage::kAttack;
            noteAgeSamples = 0;
            sweepOffsetSemitones = 0.0f;
            arpStepCounter = 0;
            arpSampleCounter = 0;
            lfsrState = 0x7FFFu;
        }
    }

    void onNoteOff(const InstrumentVoiceContext& /*ctx*/, note_t /*note*/) final
    {
        if (envStage != EnvStage::kIdle)
        {
            envStage = EnvStage::kRelease;
        }
    }

    void renderVoiceAudio(const InstrumentVoiceContext& /*ctx*/,
                          [[maybe_unused]] std::optional<AudioBufferStereoRef> inputBuffer,
                          AudioBufferStereoRef outputBuffer,
                          std::span<AudioBufferRef> /*auxOutBuffer*/) final
    {
        auto out = outputBuffer[0];
        if (envStage == EnvStage::kIdle)
        {
            std::fill(out.begin(), out.end(), 0.0f);
            return;
        }

        const float sr = sdk::kSampleRate;
        const float invSr = 1.0f / sr;

        // Envelope increments per sample
        const float attackInc =
          (ampAttackSec > 0.001f) ? (1.0f / (ampAttackSec * sr)) : 1.0f;
        const float decayDec =
          (ampDecaySec > 0.002f) ? ((1.0f - ampSustainLevel) / (ampDecaySec * sr)) : 1.0f;
        const float releaseDec =
          (ampReleaseSec > 0.002f) ? (1.0f / (ampReleaseSec * sr)) : 1.0f;

        // Portamento / glide coefficient
        const float glideStep =
          (glideTimeSec > 0.001f) ? (1.0f / (glideTimeSec * sr)) : 1.0f;

        // Arpeggio step length in samples
        const int effArpIdx =
          (model == ChipModel::kArpChord && arpChordIdx == 0) ? 3 : arpChordIdx;
        const uint32_t arpPeriodSamples =
          std::max<uint32_t>(64u, static_cast<uint32_t>(sr / std::max(5.0f, arpSpeedHz)));

        // Pitch sweep decay coefficient
        const float sweepSamples = std::max(64.0f, sweepTimeSec * sr);

        // Bitcrush quantization levels
        const bool crushActive = (bitcrushBits < 15.5f);
        const float crushScale = crushActive ? std::pow(2.0f, bitcrushBits - 1.0f) : 32768.0f;
        const float invCrushScale = 1.0f / crushScale;
        const int decimateFactor = std::clamp(static_cast<int>(std::lround(decimateRate)), 1, 32);

        // Chamberlin SVF resonance / damping
        const float qDamper = std::clamp(1.0f - 0.85f * filterResonance, 0.12f, 1.0f);

        for (size_t i = 0; i < out.size(); ++i)
        {
            // 1. Update ADSR envelope
            switch (envStage)
            {
                case EnvStage::kAttack:
                    envLevel += attackInc;
                    if (envLevel >= 1.0f)
                    {
                        envLevel = 1.0f;
                        envStage = EnvStage::kDecay;
                    }
                    break;
                case EnvStage::kDecay:
                    envLevel -= decayDec;
                    if (envLevel <= ampSustainLevel)
                    {
                        envLevel = ampSustainLevel;
                        envStage = (ampSustainLevel <= 0.0001f) ? EnvStage::kIdle
                                                                : EnvStage::kSustain;
                    }
                    break;
                case EnvStage::kSustain:
                    envLevel = ampSustainLevel;
                    break;
                case EnvStage::kRelease:
                    envLevel -= releaseDec;
                    if (envLevel <= 0.0001f)
                    {
                        envLevel = 0.0f;
                        envStage = EnvStage::kIdle;
                    }
                    break;
                case EnvStage::kIdle:
                    out[i] = 0.0f;
                    continue;
            }

            // 2. Update glide
            if (std::abs(currentGlideNote - targetGlideNote) > 0.001f)
            {
                const float diff = targetGlideNote - currentGlideNote;
                const float step = std::min(std::abs(diff), glideStep * 24.0f);
                currentGlideNote += (diff > 0.0f) ? step : -step;
            }
            else
            {
                currentGlideNote = targetGlideNote;
            }

            // 3. Update arpeggiator
            int8_t arpSemi = 0;
            if (effArpIdx > 0 && effArpIdx < 8)
            {
                if (++arpSampleCounter >= arpPeriodSamples)
                {
                    arpSampleCounter = 0;
                    arpStepCounter = (arpStepCounter + 1u) & 3u;
                }
                arpSemi = kArpChords[effArpIdx][arpStepCounter & 3u];
            }

            // 4. Update LFOs (PWM & Vibrato)
            pwmPhase += pwmRateHz * invSr;
            if (pwmPhase >= 1.0f)
            {
                pwmPhase -= 1.0f;
            }
            const float pwmLfo =
              (pwmPhase < 0.5f) ? (pwmPhase * 4.0f - 1.0f) : (3.0f - pwmPhase * 4.0f);

            vibPhase += vibRateHz * invSr;
            if (vibPhase >= 1.0f)
            {
                vibPhase -= 1.0f;
            }
            const float vibSemitones = fastSine(vibPhase) * (vibDepth * 1.5f);

            // 5. Pitch sweep envelope
            const float sweepProgress =
              std::min(1.0f, static_cast<float>(noteAgeSamples) / sweepSamples);
            float activeSweepSemi = pitchSweepSemitones * sweepProgress;
            if (model == ChipModel::kZapLaser)
            {
                // Zap/Laser has built-in rapid downward pitch drop modulated by Shape
                const float zapEnv = 1.0f - sweepProgress;
                activeSweepSemi += (12.0f + 24.0f * shape) * (zapEnv * zapEnv * zapEnv) -
                                   (12.0f + 24.0f * shape);
            }

            const float totalNote = currentGlideNote + tuneSemitones + (fineCents * 0.01f) +
                                    pitchBendSemitones + static_cast<float>(arpSemi) +
                                    vibSemitones + activeSweepSemi;
            const float freq = std::clamp(sdk::noteToFreq(0, totalNote), 16.0f, 18000.0f);
            const float dt = freq * invSr;

            // Duty cycle with PWM modulation
            const float duty = std::clamp(0.125f + 0.75f * shape + 0.38f * pwmDepth * pwmLfo,
                                          0.06f,
                                          0.94f);

            // 6. Synthesize selected voice model
            float sig = 0.0f;
            phase += dt;
            bool wrapped = false;
            if (phase >= 1.0f)
            {
                phase -= 1.0f;
                wrapped = true;
            }

            switch (model)
            {
                case ChipModel::kPulse2A03:
                case ChipModel::kArpChord:
                case ChipModel::kZapLaser:
                {
                    float pulse = (phase < duty) ? 1.0f : -1.0f;
                    pulse += polyBlep(phase, dt);
                    pulse -= polyBlep(std::fmod(phase - duty + 1.0f, 1.0f), dt);
                    sig = pulse * 0.75f;
                    break;
                }

                case ChipModel::kSidSawTri:
                {
                    float saw = 2.0f * phase - 1.0f;
                    saw -= polyBlep(phase, dt);
                    const float tri =
                      (phase < 0.5f) ? (phase * 4.0f - 1.0f) : (3.0f - phase * 4.0f);
                    // Ring-mod & waveform blend controlled by duty/shape
                    const float ring = saw * ((subPhase < 0.5f) ? 1.0f : -1.0f);
                    sig = ((1.0f - duty) * tri + duty * (0.65f * saw + 0.35f * ring)) * 0.8f;
                    break;
                }

                case ChipModel::kNoiseLfsr:
                {
                    // Clock LFSR at pitch-tracked rate
                    lfsrPhase += dt * (2.0f + 14.0f * (1.0f - pwmDepth));
                    while (lfsrPhase >= 1.0f)
                    {
                        lfsrPhase -= 1.0f;
                        lfsrState = stepLfsr(lfsrState, shape >= 0.5f);
                    }
                    sig = ((lfsrState & 1u) != 0u ? 0.8f : -0.8f);
                    break;
                }

                case ChipModel::kFmBell:
                {
                    // 2-op OPL chiptune FM: integer/bell ratios selected by Shape
                    static constexpr std::array<float, 8> kFmRatios = {
                        1.0f, 2.0f, 3.0f, 3.5f, 4.0f, 5.0f, 7.0f, 9.0f
                    };
                    const int ratioIdx =
                      std::clamp(static_cast<int>(shape * 7.99f), 0, 7);
                    const float modDt = dt * kFmRatios[ratioIdx];
                    osc2Phase += modDt;
                    if (osc2Phase >= 1.0f)
                    {
                        osc2Phase -= std::floor(osc2Phase);
                    }
                    const float modIndex =
                      (0.4f + 3.5f * (0.35f + 0.65f * pwmDepth)) * envLevel;
                    const float modSig = fastSine(osc2Phase + 0.15f * pwmLfo);
                    // Quantize FM carrier to 6-bit OPL DAC character
                    const float rawCarrier = fastSine(phase + modIndex * modSig * 0.5f);
                    sig = std::round(rawCarrier * 31.0f) / 31.0f * 0.85f;
                    break;
                }

                case ChipModel::kSuperSquare:
                {
                    // 3 detuned pulse oscillators
                    const float spread = 0.002f + 0.018f * shape;
                    const float dt2 = dt * (1.0f + spread);
                    const float dt3 = dt * (1.0f - spread);
                    osc2Phase += dt2;
                    if (osc2Phase >= 1.0f)
                    {
                        osc2Phase -= 1.0f;
                    }
                    osc3Phase += dt3;
                    if (osc3Phase >= 1.0f)
                    {
                        osc3Phase -= 1.0f;
                    }
                    float p1 = (phase < duty) ? 1.0f : -1.0f;
                    p1 += polyBlep(phase, dt) -
                          polyBlep(std::fmod(phase - duty + 1.0f, 1.0f), dt);
                    float p2 = (osc2Phase < duty) ? 1.0f : -1.0f;
                    p2 += polyBlep(osc2Phase, dt2) -
                          polyBlep(std::fmod(osc2Phase - duty + 1.0f, 1.0f), dt2);
                    float p3 = (osc3Phase < duty) ? 1.0f : -1.0f;
                    p3 += polyBlep(osc3Phase, dt3) -
                          polyBlep(std::fmod(osc3Phase - duty + 1.0f, 1.0f), dt3);
                    sig = (p1 + 0.8f * p2 + 0.8f * p3) * 0.34f;
                    break;
                }

                case ChipModel::kChipDrum:
                {
                    // Morphs from Kick (0..0.33) -> Snare (0.33..0.67) -> Metallic Hat (0.67..1.0)
                    const float drumDecay =
                      std::exp(-static_cast<float>(noteAgeSamples) / (0.08f * sr));
                    const float drumFreq =
                      freq * (1.0f + 3.5f * (1.0f - shape * 0.7f) * drumDecay * drumDecay);
                    osc2Phase += drumFreq * invSr;
                    if (osc2Phase >= 1.0f)
                    {
                        osc2Phase -= std::floor(osc2Phase);
                    }
                    if (wrapped || (noteAgeSamples & 1u) == 0u)
                    {
                        lfsrState = stepLfsr(lfsrState, shape > 0.75f);
                    }
                    const float body = nesSteppedTriangle(osc2Phase);
                    const float noise = ((lfsrState & 1u) != 0u) ? 0.85f : -0.85f;
                    const float noiseMix =
                      (noteAgeSamples < 180u) ? 0.7f : std::clamp(shape * 1.25f, 0.0f, 1.0f);
                    sig = (1.0f - noiseMix) * body + noiseMix * noise;
                    break;
                }
            }

            // 7. Add 4-bit stepped NES Sub-Oscillator (-1 octave)
            subPhase += dt * 0.5f;
            if (subPhase >= 1.0f)
            {
                subPhase -= 1.0f;
            }
            if (subMix > 0.001f)
            {
                sig = sig * (1.0f - 0.35f * subMix) + nesSteppedTriangle(subPhase) * (0.75f * subMix);
            }

            // 8. Sample-rate decimation & bitcrusher
            if (++decimateCounter >= decimateFactor)
            {
                decimateCounter = 0;
                if (crushActive)
                {
                    heldSample = std::round(sig * crushScale) * invCrushScale;
                }
                else
                {
                    heldSample = sig;
                }
            }
            float postCrush = heldSample;

            // 9. State-Variable Lowpass Filter with envelope modulation
            const float envCutoff =
              std::clamp(filterCutoffHz *
                           std::pow(2.0f, filterEnvAmount * envLevel * 4.0f),
                         40.0f,
                         18000.0f);
            if (envCutoff < 17500.0f || filterResonance > 0.02f)
            {
                const float f =
                  std::min(0.85f, 2.0f * std::sin(std::numbers::pi_v<float> * envCutoff * invSr));
                svfLow += f * svfBand;
                const float high = postCrush - svfLow - qDamper * svfBand;
                svfBand += f * high;
                svfBand = std::clamp(svfBand, -2.0f, 2.0f);
                svfLow = std::clamp(svfLow, -2.0f, 2.0f);
                postCrush = svfLow;
            }

            out[i] = postCrush * envLevel * volumeMult;
            ++noteAgeSamples;
        }
    }

    static void createMetadata(const Patch& /*patch*/, Patch::Metadata& metadata)
    {
        using namespace polysynth::sdk;

        metadata.params.resize(enumutil::EnumMax<ChipParamId>::value + 1);
        metadata.params.shrink_to_fit();

        if (paramHandlers.empty())
        {
            paramHandlers.resize(enumutil::EnumMax<ChipParamId>::value + 1);
        }

        ParamHelper<ChiptuneSynthVoice> helper(metadata, paramHandlers);

        helper.addParam(
          *ChipParamId::kModel,
          {
            .displayName = "Model",
            .defaultValue = 0,
            .minValue = 0,
            .maxValue = static_cast<Patch::paramvalue_short_t>(kModelNames.size() - 1),
            .isEnum = true,
            .midiCC = *MidiCCs::kOscParam0,
            .valueFormatter =
              [](const Patch& /*patch*/, Patch::paramvalue_t value)
            {
                const int idx =
                  std::clamp<int>(static_cast<int>(std::lround(value)), 0, kModelNames.size() - 1);
                return std::string(kModelNames[idx]);
            },
          },
          [](auto& voice, Patch::optional_paramvalue_t val)
          {
              voice.model = static_cast<ChipModel>(
                std::clamp<int>(static_cast<int>(std::lround(*val)), 0, kModelNames.size() - 1));
          });

        helper.addParam(
          *ChipParamId::kTune,
          {
            .displayName = "Tune",
            .defaultValue = 0.0f,
            .minValue = -24,
            .maxValue = 24,
            .midiCC = *MidiCCs::kOscParam0 + 1,
            .valueFormatter = createSemitoneFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val)
          { voice.tuneSemitones = std::round(*val); });

        helper.addParam(
          *ChipParamId::kFine,
          {
            .displayName = "Fine",
            .defaultValue = 0.0f,
            .minValue = -100,
            .maxValue = 100,
            .midiCC = *MidiCCs::kOscParam0 + 2,
            .valueFormatter = createCentsFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.fineCents = *val; });

        helper.addParam(
          *ChipParamId::kShape,
          {
            .displayName = "Duty / Shape",
            .defaultValue = 0.35f,
            .minValue = 0,
            .maxValue = 1,
            .midiCC = *MidiCCs::kOscParam0 + 3,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.shape = *val; });

        helper.addParam(
          *ChipParamId::kPwmRate,
          {
            .displayName = "PWM Rate",
            .defaultValue = 2.5f,
            .minValue = 0.1f,
            .maxValue = 40.0f,
            .editCurve = Patch::Metadata::Param::Curve::kExponential,
            .midiCC = *MidiCCs::kOscParam0 + 4,
            .valueFormatter = createFrequencyFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.pwmRateHz = *val; });

        helper.addParam(
          *ChipParamId::kPwmDepth,
          {
            .displayName = "PWM Depth",
            .defaultValue = 0.2f,
            .minValue = 0,
            .maxValue = 1,
            .midiCC = *MidiCCs::kOscParam0 + 5,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.pwmDepth = *val; });

        helper.addParam(
          *ChipParamId::kSubMix,
          {
            .displayName = "Sub Osc",
            .defaultValue = 0.25f,
            .minValue = 0,
            .maxValue = 1,
            .midiCC = *MidiCCs::kOscParam0 + 6,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.subMix = *val; });

        helper.addParam(
          *ChipParamId::kArpChord,
          {
            .displayName = "Chord Arp",
            .defaultValue = 0,
            .minValue = 0,
            .maxValue = static_cast<Patch::paramvalue_short_t>(kArpNames.size() - 1),
            .isEnum = true,
            .midiCC = *MidiCCs::kOscParam0 + 7,
            .valueFormatter =
              [](const Patch& /*patch*/, Patch::paramvalue_t value)
            {
                const int idx =
                  std::clamp<int>(static_cast<int>(std::lround(value)), 0, kArpNames.size() - 1);
                return std::string(kArpNames[idx]);
            },
          },
          [](auto& voice, Patch::optional_paramvalue_t val)
          {
              voice.arpChordIdx =
                std::clamp<int>(static_cast<int>(std::lround(*val)), 0, kArpNames.size() - 1);
          });

        helper.addParam(
          *ChipParamId::kArpSpeed,
          {
            .displayName = "Arp Speed",
            .defaultValue = 50.0f,
            .minValue = 5.0f,
            .maxValue = 75.0f,
            .midiCC = *MidiCCs::kOscParam0 + 8,
            .valueFormatter = createFrequencyFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.arpSpeedHz = *val; });

        helper.addParam(
          *ChipParamId::kPitchSweep,
          {
            .displayName = "Pitch Sweep",
            .defaultValue = 0.0f,
            .minValue = -36.0f,
            .maxValue = 36.0f,
            .midiCC = *MidiCCs::kOscParam0 + 9,
            .valueFormatter = createSemitoneFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val)
          { voice.pitchSweepSemitones = *val; });

        helper.addParam(
          *ChipParamId::kSweepTime,
          {
            .displayName = "Sweep Time",
            .defaultValue = 0.25f,
            .minValue = 0.02f,
            .maxValue = 2.0f,
            .editCurve = Patch::Metadata::Param::Curve::kExponential,
            .midiCC = *MidiCCs::kOscParam0 + 10,
            .valueFormatter = createTimeFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.sweepTimeSec = *val; });

        helper.addParam(
          *ChipParamId::kVibRate,
          {
            .displayName = "Vibrato Rate",
            .defaultValue = 6.0f,
            .minValue = 0.5f,
            .maxValue = 25.0f,
            .editCurve = Patch::Metadata::Param::Curve::kExponential,
            .midiCC = *MidiCCs::kLfo1Frequency,
            .valueFormatter = createFrequencyFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.vibRateHz = *val; });

        helper.addParam(
          *ChipParamId::kVibDepth,
          {
            .displayName = "Vibrato Depth",
            .defaultValue = 0.0f,
            .minValue = 0.0f,
            .maxValue = 1.0f,
            .midiCC = *MidiCCs::kOscParam0 + 11,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.vibDepth = *val; });

        helper.addParam(
          *ChipParamId::kAmpAttack,
          {
            .displayName = "Attack",
            .defaultValue = 0.0f,
            .minValue = 0.0f,
            .maxValue = 5.0f,
            .editCurve = Patch::Metadata::Param::Curve::kExponential,
            .midiCC = *MidiCCs::kAmpEnvAttack,
            .valueFormatter = createTimeFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.ampAttackSec = *val; });

        helper.addParam(
          *ChipParamId::kAmpDecay,
          {
            .displayName = "Decay",
            .defaultValue = 0.35f,
            .minValue = 0.01f,
            .maxValue = 5.0f,
            .editCurve = Patch::Metadata::Param::Curve::kExponential,
            .midiCC = *MidiCCs::kAmpEnvDecay,
            .valueFormatter = createTimeFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.ampDecaySec = *val; });

        helper.addParam(
          *ChipParamId::kAmpSustain,
          {
            .displayName = "Sustain",
            .defaultValue = 0.4f,
            .minValue = 0.0f,
            .maxValue = 1.0f,
            .midiCC = *MidiCCs::kAmpEnvSustain,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.ampSustainLevel = *val; });

        helper.addParam(
          *ChipParamId::kAmpRelease,
          {
            .displayName = "Release",
            .defaultValue = 0.15f,
            .minValue = 0.01f,
            .maxValue = 5.0f,
            .editCurve = Patch::Metadata::Param::Curve::kExponential,
            .midiCC = *MidiCCs::kAmpEnvRelease,
            .valueFormatter = createTimeFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.ampReleaseSec = *val; });

        helper.addParam(
          *ChipParamId::kFilterCutoff,
          {
            .displayName = "Cutoff",
            .defaultValue = 12000.0f,
            .minValue = 80.0f,
            .maxValue = 18000.0f,
            .editCurve = Patch::Metadata::Param::Curve::kExponentialSteep,
            .midiCC = *MidiCCs::kFilterCutoff,
            .valueFormatter = createFrequencyFormatter(18000.0f),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.filterCutoffHz = *val; });

        helper.addParam(
          *ChipParamId::kFilterResonance,
          {
            .displayName = "Resonance",
            .defaultValue = 0.15f,
            .minValue = 0.0f,
            .maxValue = 1.0f,
            .midiCC = *MidiCCs::kFilterResonance,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.filterResonance = *val; });

        helper.addParam(
          *ChipParamId::kFilterEnvAmount,
          {
            .displayName = "Env Amt",
            .defaultValue = 0.2f,
            .minValue = -1.0f,
            .maxValue = 1.0f,
            .midiCC = *MidiCCs::kFilterEnvAmount,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.filterEnvAmount = *val; });

        helper.addParam(
          *ChipParamId::kBitcrush,
          {
            .displayName = "Bitcrush",
            .defaultValue = 8.0f,
            .minValue = 2.0f,
            .maxValue = 16.0f,
            .valueFormatter =
              [](const Patch& /*patch*/, Patch::paramvalue_t value)
            { return fmt::format("{:.0f} bit", value); },
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.bitcrushBits = *val; });

        helper.addParam(
          *ChipParamId::kDecimate,
          {
            .displayName = "Decimate",
            .defaultValue = 1.0f,
            .minValue = 1.0f,
            .maxValue = 32.0f,
            .valueFormatter =
              [](const Patch& /*patch*/, Patch::paramvalue_t value)
            { return fmt::format("{:.0f}x", std::round(value)); },
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.decimateRate = *val; });

        helper.addParam(
          *ChipParamId::kPlayMode,
          {
            .displayName = "Glide Mode",
            .defaultValue = 0,
            .minValue = 0,
            .maxValue = 2,
            .isEnum = true,
            .valueFormatter =
              [](const Patch& /*patch*/, Patch::paramvalue_t value)
            {
                constexpr std::array kModes = { "Always", "Overlap", "Legato" };
                return std::string(kModes.at(std::clamp<int>(std::lround(value), 0, 2)));
            },
          },
          [](auto& voice, Patch::optional_paramvalue_t val)
          { voice.playMode = static_cast<PlayMode>(std::clamp<int>(std::lround(*val), 0, 2)); });

        helper.addParam(
          *ChipParamId::kGlideTime,
          {
            .displayName = "Glide Time",
            .defaultValue = 0.0f,
            .minValue = 0.0f,
            .maxValue = 2.0f,
            .editCurve = Patch::Metadata::Param::Curve::kExponential,
            .valueFormatter = createTimeFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.glideTimeSec = *val; });

        helper.addParam(
          *ChipParamId::kVolume,
          {
            .displayName = "Volume",
            .defaultValue = 1.0f,
            .minValue = 0.0f,
            .maxValue = 2.0f,
            .macroOperation = Patch::Metadata::Param::MacroOperation::kMult,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.volumeMult = *val; });

        metadata.layoutFlat = Patch::Metadata::EditorLayout{
            .sections = {
              {
                .name = "Oscillator",
                .shortName = "Osc",
                .elements = {
                  { .type = EditorElement::Type::kParam, .id = *ChipParamId::kModel },
                  { .type = EditorElement::Type::kParam, .id = *ChipParamId::kTune },
                  { .type = EditorElement::Type::kParam, .id = *ChipParamId::kFine },
                  { .type = EditorElement::Type::kParam, .id = *ChipParamId::kShape },
                  { .type = EditorElement::Type::kParam, .id = *ChipParamId::kPwmRate },
                  { .type = EditorElement::Type::kParam, .id = *ChipParamId::kPwmDepth },
                  { .type = EditorElement::Type::kParam, .id = *ChipParamId::kSubMix },
                },
              },
              {
                .name = "Modulation",
                .shortName = "Mod",
                .elements = {
                  { .type = EditorElement::Type::kParam, .id = *ChipParamId::kArpChord },
                  { .type = EditorElement::Type::kParam, .id = *ChipParamId::kArpSpeed },
                  { .type = EditorElement::Type::kParam, .id = *ChipParamId::kPitchSweep },
                  { .type = EditorElement::Type::kParam, .id = *ChipParamId::kSweepTime },
                  { .type = EditorElement::Type::kParam, .id = *ChipParamId::kVibRate },
                  { .type = EditorElement::Type::kParam, .id = *ChipParamId::kVibDepth },
                },
              },
              {
                .name = "Amplifier",
                .shortName = "Amp",
                .elements = {
                  { .type = EditorElement::Type::kParam, .id = *ChipParamId::kAmpAttack },
                  { .type = EditorElement::Type::kParam, .id = *ChipParamId::kAmpDecay },
                  { .type = EditorElement::Type::kParam, .id = *ChipParamId::kAmpSustain },
                  { .type = EditorElement::Type::kParam, .id = *ChipParamId::kAmpRelease },
                },
              },
              {
                .name = "Filter",
                .shortName = "Filter",
                .elements = {
                  { .type = EditorElement::Type::kParam, .id = *ChipParamId::kFilterCutoff },
                  { .type = EditorElement::Type::kParam, .id = *ChipParamId::kFilterResonance },
                  { .type = EditorElement::Type::kParam, .id = *ChipParamId::kFilterEnvAmount },
                  { .type = EditorElement::Type::kParam, .id = *ChipParamId::kBitcrush },
                  { .type = EditorElement::Type::kParam, .id = *ChipParamId::kDecimate },
                },
              },
              {
                .name = "Voice",
                .shortName = "Voice",
                .elements = {
                  { .type = EditorElement::Type::kParam, .id = *ChipParamId::kPlayMode },
                  { .type = EditorElement::Type::kParam, .id = *ChipParamId::kGlideTime },
                  { .type = EditorElement::Type::kParam, .id = *ChipParamId::kVolume },
                },
              },
            },
        };

        metadata.layout23 = metadata.layoutFlat;

        metadata.initDefaultMacros = [](auto& macros)
        {
            macros[0] = Patch::Metadata::Macro{
                .displayName = "Timbre / PWM",
                .modifiers = {
                  { *ChipParamId::kShape, 0.7f },
                  { *ChipParamId::kPwmDepth, 0.5f },
                },
                .bipolar = true,
            };
            macros[1] = Patch::Metadata::Macro{
                .displayName = "Arp / Chord",
                .modifiers = {
                  { *ChipParamId::kArpChord, 1.0f },
                },
                .bipolar = false,
            };
            macros[2] = Patch::Metadata::Macro{
                .displayName = "Sweep / Vibrato",
                .modifiers = {
                  { *ChipParamId::kVibDepth, 0.6f },
                  { *ChipParamId::kPitchSweep, 0.5f },
                },
                .bipolar = true,
            };
            macros[3] = Patch::Metadata::Macro{
                .displayName = "Env Decay / Rel",
                .modifiers = {
                  { *ChipParamId::kAmpDecay, 0.8f },
                  { *ChipParamId::kAmpRelease, 0.6f },
                },
                .bipolar = true,
            };
            macros[4] = Patch::Metadata::Macro{
                .displayName = "Crush / Lo-Fi",
                .modifiers = {
                  { *ChipParamId::kBitcrush, -0.6f },
                  { *ChipParamId::kDecimate, 0.4f },
                },
                .bipolar = false,
            };
            macros[5] = Patch::Metadata::Macro{
                .displayName = "Filter Cutoff",
                .modifiers = {
                  { *ChipParamId::kFilterCutoff, 1.0f },
                  { *ChipParamId::kFilterResonance, 0.35f },
                },
                .bipolar = true,
            };

            macros[*Patch::SystemMacroId::kVelocity].modifiers = {
                { *ChipParamId::kVolume, 1.0f },
            };
        };

        metadata.instrumentTypeId = static_cast<int>(InstrumentTypeId::kChiptuneSynth);
        metadata.outputChannels = Patch::Metadata::OutputChannels::kMono;
    }

  private:
    enum class EnvStage : uint8_t
    {
        kIdle = 0,
        kAttack,
        kDecay,
        kSustain,
        kRelease,
    };

    ChipModel model = ChipModel::kPulse2A03;
    float tuneSemitones = 0.0f;
    float fineCents = 0.0f;
    float shape = 0.35f;
    float pwmRateHz = 2.5f;
    float pwmDepth = 0.2f;
    float subMix = 0.25f;

    int arpChordIdx = 0;
    float arpSpeedHz = 50.0f;
    float pitchSweepSemitones = 0.0f;
    float sweepTimeSec = 0.25f;
    float vibRateHz = 6.0f;
    float vibDepth = 0.0f;

    float ampAttackSec = 0.0f;
    float ampDecaySec = 0.35f;
    float ampSustainLevel = 0.4f;
    float ampReleaseSec = 0.15f;

    float filterCutoffHz = 12000.0f;
    float filterResonance = 0.15f;
    float filterEnvAmount = 0.2f;
    float bitcrushBits = 8.0f;
    float decimateRate = 1.0f;

    float glideTimeSec = 0.0f;
    float volumeMult = 1.0f;

    // Real-time voice state
    EnvStage envStage = EnvStage::kIdle;
    float envLevel = 0.0f;
    bool hasPlayedAnyNote = false;
    float currentGlideNote = 60.0f;
    float targetGlideNote = 60.0f;

    float phase = 0.0f;
    float osc2Phase = 0.0f;
    float osc3Phase = 0.0f;
    float subPhase = 0.0f;
    float pwmPhase = 0.0f;
    float vibPhase = 0.0f;
    float lfsrPhase = 0.0f;
    uint16_t lfsrState = 0x7FFFu;

    uint32_t noteAgeSamples = 0;
    uint32_t arpSampleCounter = 0;
    uint32_t arpStepCounter = 0;
    float sweepOffsetSemitones = 0.0f;

    int decimateCounter = 0;
    float heldSample = 0.0f;
    float svfLow = 0.0f;
    float svfBand = 0.0f;
};

} // namespace

std::unique_ptr<InstrumentType> createChiptuneSynth()
{
    return std::make_unique<InstrumentType>(
      InstrumentType{ .name = "CHIP",
                      .createMetadata = &ChiptuneSynthVoice::createMetadata,
                      .createVoice = [](int /*voiceIdx*/) -> std::unique_ptr<InstrumentVoice>
                      { return std::make_unique<ChiptuneSynthVoice>(); } });
}

} // namespace polysynth

#endif
