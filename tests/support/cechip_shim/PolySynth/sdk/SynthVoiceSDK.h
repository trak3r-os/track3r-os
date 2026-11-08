// SPDX-License-Identifier: GPL-3.0-or-later
//
// The oracle shim of OS-CECHIP C1: the smallest stand-in for the header named "PolySynth/sdk/SynthVoiceSDK.h" that tracker-ce's
// ChiptuneSynthVoice.cpp includes, so that the original compiles UNMODIFIED on the host. Ours, written from the facts of
// docs/specs/OS-CECHIP.md (F5: noteToFreq; F8: the voice ignores velocity, play modes Always/Overlap/Legato, pitch bend) and from what the original's
// own source uses; no code of tracker-ce's SDK is copied (the spec's rule A-10), and the SDK header's contents are not reproduced. Where the
// original needs a name whose value does not matter to its audio (MIDI CC numbers, editor layouts, macros, value formatters) the shim keeps the
// name and a plain value. What matters to the audio, and is therefore stated from the facts:
//   - kSampleRate (44,100 Hz, this project's audio clock);
//   - noteToFreq(note, detune) = 440 * 2^((note - 69 + detune) / 12), in single precision, the expression F5 states;
//   - the parameter setters run on the voice through setParam(id, value), the way the original's handler table is built (ParamHelper).
// C++20 (the original uses designated initialisers, std::span and std::numbers); host only; never part of an image.
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// The original calls fmt::format("{:.0f} bit", x) and fmt::format("{:.0f}x", x): the one specifier "{:.Nf}", which this stand-in supports.
namespace fmt {
inline std::string format(const char* spec, float value) {
  const std::string_view s(spec);
  const auto open = s.find("{:.");
  const auto close = s.find("f}", open);
  if (open == std::string_view::npos || close == std::string_view::npos) {
    return std::string(s);
  }
  const int digits = std::atoi(std::string(s.substr(open + 3, close - open - 3)).c_str());
  char buf[48];
  std::snprintf(buf, sizeof buf, "%.*f", digits, static_cast<double>(value));
  return std::string(s.substr(0, open)) + buf + std::string(s.substr(close + 2));
}
}  // namespace fmt

namespace polysynth {

using note_t = int;

// What the voice receives with every call: nothing the CHIP engine reads.
struct InstrumentVoiceContext {};

// One channel of audio, and a stereo pair of them. The original writes channel 0 only.
using AudioBufferRef = std::span<float>;
using AudioBufferStereoRef = std::array<AudioBufferRef, 2>;

enum class InstrumentTypeId : int { kChiptuneSynth = 0 };

// A voice: the four calls the original overrides, plus the shim's way of delivering a parameter value.
class InstrumentVoice {
 public:
  virtual ~InstrumentVoice() = default;
  virtual bool isActive() = 0;
  virtual void onNoteOn(const InstrumentVoiceContext& ctx, note_t note, float velocity, bool isLegatoStep) = 0;
  virtual void onNoteOff(const InstrumentVoiceContext& ctx, note_t note) = 0;
  virtual void renderVoiceAudio(const InstrumentVoiceContext& ctx, std::optional<AudioBufferStereoRef> inputBuffer,
                                AudioBufferStereoRef outputBuffer, std::span<AudioBufferRef> auxOutBuffer) = 0;
  virtual void setParam(int id, float value) = 0;
};

// An editor element and its layout: only the shape of the original's aggregates.
struct EditorElement {
  enum class Type : int { kParam = 0 };
  Type type = Type::kParam;
  int id = 0;
};
struct EditorSection {
  const char* name = "";
  const char* shortName = "";
  std::vector<EditorElement> elements;
};

struct Patch {
  using paramvalue_t = float;
  using paramvalue_short_t = std::int16_t;
  using optional_paramvalue_t = std::optional<float>;
  enum class SystemMacroId : int { kVelocity = 6 };

  struct Metadata {
    struct Param {
      enum class Curve : int { kLinear = 0, kExponential, kExponentialSteep };
      enum class MacroOperation : int { kAdd = 0, kMult };
      const char* displayName = "";
      paramvalue_t defaultValue = 0.0f;
      paramvalue_t minValue = 0.0f;
      paramvalue_t maxValue = 1.0f;
      bool isEnum = false;
      Curve editCurve = Curve::kLinear;
      int midiCC = -1;
      MacroOperation macroOperation = MacroOperation::kAdd;
      std::function<std::string(const Patch&, paramvalue_t)> valueFormatter;
    };
    struct EditorLayout {
      std::vector<EditorSection> sections;
    };
    struct Modifier {
      int paramId = 0;
      float amount = 0.0f;
    };
    struct Macro {
      const char* displayName = "";
      std::vector<Modifier> modifiers;
      bool bipolar = false;
    };
    enum class OutputChannels : int { kMono = 0, kStereo };
    using MacroArray = std::array<Macro, 8>;

    std::vector<Param> params;
    EditorLayout layoutFlat;
    EditorLayout layout23;
    std::function<void(MacroArray&)> initDefaultMacros;
    int instrumentTypeId = 0;
    OutputChannels outputChannels = OutputChannels::kStereo;
  };
};

// An instrument type: a name, the metadata builder and the voice factory.
struct InstrumentType {
  const char* name = "";
  void (*createMetadata)(const Patch&, Patch::Metadata&) = nullptr;
  std::function<std::unique_ptr<InstrumentVoice>(int)> createVoice;
};

namespace sdk {

// The audio clock of this project (O3OS_RATE_44100). CE's own engine reads its rate from here.
inline constexpr float kSampleRate = 44100.0f;

// Fact F5: 440 Hz at note 69, an octave per twelve semitones, the detune in semitones, in single precision.
inline float noteToFreq(note_t note, float detuneSemitones = 0.0f) {
  return 440.0f * std::pow(2.0f, (static_cast<float>(note - 69) + detuneSemitones) / 12.0f);
}

// MIDI CC names the original assigns to its parameters. The numbers are placeholders: nothing reads them.
enum class MidiCCs : int {
  kOscParam0 = 20,
  kLfo1Frequency = 40,
  kAmpEnvAttack,
  kAmpEnvDecay,
  kAmpEnvSustain,
  kAmpEnvRelease,
  kFilterCutoff,
  kFilterResonance,
  kFilterEnvAmount,
};

// The original writes `*Enumerator` for the value of a scoped enumerator.
template <class E, std::enable_if_t<std::is_enum_v<E>, int> = 0>
constexpr std::underlying_type_t<E> operator*(E e) {
  return static_cast<std::underlying_type_t<E>>(e);
}

namespace enumutil {
namespace detail {
// Whether the enumerator value V has a name (the compiler prints an unnamed one as a cast, "(Type)25").
template <auto V>
constexpr bool named() {
  constexpr std::string_view s = __PRETTY_FUNCTION__;
  constexpr auto at = s.find("V = ");
  static_assert(at != std::string_view::npos, "unexpected __PRETTY_FUNCTION__ layout");
  return s[at + 4] != '(';
}
template <class E, int... I>
constexpr int max_named(std::integer_sequence<int, I...>) {
  int m = -1;
  ((named<static_cast<E>(I)>() ? (m = I) : 0), ...);
  return m;
}
}  // namespace detail

// The largest value among the enumerators of E (ids 0..63, contiguous from 0).
template <class E>
struct EnumMax {
  static constexpr int value = detail::max_named<E>(std::make_integer_sequence<int, 64>{});
};
}  // namespace enumutil

// Value formatters: what the editor would print. Plain text; nothing in the audio reads them.
using ValueFormatter = std::function<std::string(const Patch&, Patch::paramvalue_t)>;
inline std::string format_value(const char* spec, float v) { return fmt::format(spec, v); }
inline ValueFormatter createPercentFormatter() {
  return [](const Patch&, Patch::paramvalue_t v) { return format_value("{:.0f}%", v * 100.0f); };
}
inline ValueFormatter createSemitoneFormatter() {
  return [](const Patch&, Patch::paramvalue_t v) { return format_value("{:.0f} st", v); };
}
inline ValueFormatter createCentsFormatter() {
  return [](const Patch&, Patch::paramvalue_t v) { return format_value("{:.0f} ct", v); };
}
inline ValueFormatter createFrequencyFormatter(float /*max*/ = 0.0f) {
  return [](const Patch&, Patch::paramvalue_t v) { return format_value("{:.1f} Hz", v); };
}
inline ValueFormatter createTimeFormatter() {
  return [](const Patch&, Patch::paramvalue_t v) { return format_value("{:.3f} s", v); };
}

// Registers a parameter's description and its setter, indexed by id.
template <class Voice>
class ParamHelper {
 public:
  using Handler = std::function<void(Voice&, Patch::optional_paramvalue_t)>;
  ParamHelper(Patch::Metadata& metadata, std::vector<Handler>& handlers) : metadata_(metadata), handlers_(handlers) {}
  void addParam(int id, Patch::Metadata::Param param, Handler handler) {
    metadata_.params.at(static_cast<std::size_t>(id)) = std::move(param);
    handlers_.at(static_cast<std::size_t>(id)) = std::move(handler);
  }

 private:
  Patch::Metadata& metadata_;
  std::vector<Handler>& handlers_;
};

// The base of a voice: the glide mode and the pitch bend the original reads, and the handler table its createMetadata fills.
template <class Derived, class ParamId>
class CustomSynthVoice : public InstrumentVoice {
 public:
  enum class PlayMode : int { kAlways = 0, kOverlap, kLegato };
  using Handler = std::function<void(Derived&, Patch::optional_paramvalue_t)>;

  static inline std::vector<Handler> paramHandlers;

  void setParam(int id, float value) final {
    if (id >= 0 && static_cast<std::size_t>(id) < paramHandlers.size() && paramHandlers[static_cast<std::size_t>(id)]) {
      paramHandlers[static_cast<std::size_t>(id)](static_cast<Derived&>(*this), value);
    }
  }

  PlayMode playMode = PlayMode::kAlways;
  float pitchBendSemitones = 0.0f;  // the oracle never bends
};

}  // namespace sdk
}  // namespace polysynth
