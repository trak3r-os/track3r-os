// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/sim/scenario_assert.h>

#include <o3os/core/byte_io.h>
#include <o3os/core/param.h>
#include <o3os/formats/native.h>
#include <o3os/model/instrument.h>
#include <o3os/model/limits.h>
#include <o3os/model/params.h>
#include <o3os/model/pattern.h>
#include <o3os/platform/host/sha256.h>

#include <regex.h>

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>

namespace o3os::sim {

namespace {

// POSIX extended regular expressions (regcomp), because the build has no exceptions and std::regex throws on a bad pattern.
class Regex {
 public:
  explicit Regex(const std::string& pattern) { ok_ = ::regcomp(&re_, pattern.c_str(), REG_EXTENDED | REG_NOSUB) == 0; }
  ~Regex() {
    if (ok_) {
      ::regfree(&re_);
    }
  }
  Regex(const Regex&) = delete;
  Regex& operator=(const Regex&) = delete;
  bool ok() const { return ok_; }
  bool search(const std::string& text) const { return ok_ && ::regexec(&re_, text.c_str(), 0, nullptr, 0) == 0; }

 private:
  regex_t re_{};
  bool ok_ = false;
};

std::string lower(std::string s) {
  for (char& c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

// A whole-string unsigned decimal in [lo, hi].
bool parse_uint(const std::string& s, unsigned long lo, unsigned long hi, unsigned long& out) {
  if (s.empty() || s.size() > 10) {
    return false;
  }
  unsigned long v = 0;
  for (const char c : s) {
    if (c < '0' || c > '9') {
      return false;
    }
    v = v * 10u + static_cast<unsigned long>(c - '0');
  }
  if (v < lo || v > hi) {
    return false;
  }
  out = v;
  return true;
}

std::string quoted(const std::string& s) { return "'" + s + "'"; }

std::string number_text(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, std::fabs(v) < 1e15 && v == std::floor(v) ? "%.0f" : "%g", v);  // a whole number in full (4294967295), the rest to six digits
  return buf;
}

// ---- assert state --------------------------------------------------------------------------------------------------------------------

// o3hil.py parse_pairs: every whitespace-separated word with an "=" is key=value (split at the first one); the first of a repeated key stays.
std::map<std::string, std::string> parse_pairs(const std::string& line) {
  std::map<std::string, std::string> out;
  std::istringstream words(line);
  for (std::string word; words >> word;) {
    const std::size_t eq = word.find('=');
    if (eq != std::string::npos) {
      out.emplace(word.substr(0, eq), word.substr(eq + 1));
    }
  }
  return out;
}

// ---- assert model --------------------------------------------------------------------------------------------------------------------

struct ModelPath {
  enum class Kind : std::uint8_t { Tempo, Name, OrderLen, Order, Step, InstName, InstParam };
  enum class StepField : std::uint8_t { Note, Instr, Vol, Fx1, Fx2 };
  Kind kind = Kind::Tempo;
  StepField step_field = StepField::Note;
  unsigned a = 0;      // Order: the position; Step: the pattern id; InstName, InstParam: the instrument number (1..64)
  unsigned track = 0;  // Step
  unsigned step = 0;   // Step
  unsigned field = 0;  // InstParam: the field id of model/params.h
};

struct ParamName {
  std::string caption;
  unsigned field;
};

// The caption of a row in lower case with every run of other characters as one "_" ("PB RANGE" -> "pb_range").
std::string caption_of(const char* name) {
  std::string out;
  for (const char* p = name; *p != '\0'; ++p) {
    if (std::isalnum(static_cast<unsigned char>(*p))) {
      out += static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
    } else if (!out.empty() && out.back() != '_') {
      out += '_';
    }
  }
  while (!out.empty() && out.back() == '_') {
    out.pop_back();
  }
  return out;
}

const std::vector<ParamName>& param_names() {
  static const std::vector<ParamName> table = [] {
    namespace ip = model::instrument_param;
    std::vector<ParamName> names;
    const auto add = [&names](const std::string& prefix, unsigned field) {
      const core::ParamDesc* desc = model::param_desc(core::param_id(core::ParamKind::Instrument, 0, field));
      if (field == ip::MidiChannel) {
        desc = &model::midi_channel_desc();  // (the one virtual field: param_desc has nothing for it)
      }
      if (field == ip::PlayStereo) {
        desc = &model::play_stereo_desc();  // (the other virtual field, OS-STEREO step 2: the instrument's own byte)
      }
      if (desc != nullptr) {
        names.push_back({prefix + caption_of(desc->name), field});
      }
    };
    for (unsigned f = 0; f < ip::kBase; ++f) {
      add("", f);
    }
    for (unsigned f = ip::kSourceFirst; f < ip::kSourceFirst + ip::kSourceRows; ++f) {
      add("", f);
    }
    add("", ip::MidiChannel);
    add("", ip::PlayStereo);
    for (unsigned e = 0; e < ip::kEnvelopes; ++e) {
      for (unsigned f = 0; f < ip::kEnvelopeFields; ++f) {
        add("env" + std::to_string(e) + ".", ip::envelope(e, f));
      }
      for (unsigned f = 0; f < ip::kModFields; ++f) {
        add("mod" + std::to_string(e) + ".", ip::mod(e, f));
      }
    }
    for (unsigned f = ip::kMidiFirst; f < ip::kMidiFirst + ip::kMidiRows; ++f) {
      add("", f);
    }
    return names;
  }();
  return table;
}

bool parse_model_path(const std::string& path, ModelPath& out, std::string& error) {
  std::vector<std::string> part;
  for (std::size_t at = 0;;) {
    const std::size_t dot = path.find('.', at);
    part.push_back(path.substr(at, dot == std::string::npos ? std::string::npos : dot - at));
    if (dot == std::string::npos) {
      break;
    }
    at = dot + 1;
  }
  unsigned long n = 0;
  const auto bad = [&](const std::string& what) {
    error = what + " in model path " + quoted(path);
    return false;
  };
  if (path == "tempo" || path == "name") {
    out.kind = path == "tempo" ? ModelPath::Kind::Tempo : ModelPath::Kind::Name;
    return true;
  }
  if (part[0] == "order" && part.size() == 2) {
    if (part[1] == "len") {
      out.kind = ModelPath::Kind::OrderLen;
      return true;
    }
    if (!parse_uint(part[1], 0, 255, n)) {
      return bad("the song position must be 0..255 or 'len'");
    }
    out.kind = ModelPath::Kind::Order;
    out.a = static_cast<unsigned>(n);
    return true;
  }
  if (part[0] == "pat" && part.size() == 5) {
    out.kind = ModelPath::Kind::Step;
    if (!parse_uint(part[1], 1, model::kMaxPatternId, n)) {
      return bad("the pattern must be 1..255");
    }
    out.a = static_cast<unsigned>(n);
    if (!parse_uint(part[2], 0, model::kMaxTracks - 1u, n)) {
      return bad("the track must be 0..15");
    }
    out.track = static_cast<unsigned>(n);
    if (!parse_uint(part[3], 0, model::kMaxPatternLength - 1u, n)) {
      return bad("the step must be 0..127");
    }
    out.step = static_cast<unsigned>(n);
    const std::string f = part[4];
    if (f == "note" || f == "instr" || f == "vol" || f == "fx1" || f == "fx2") {
      out.step_field = f == "note" ? ModelPath::StepField::Note
                       : f == "instr" ? ModelPath::StepField::Instr
                       : f == "vol"   ? ModelPath::StepField::Vol
                       : f == "fx1"   ? ModelPath::StepField::Fx1
                                      : ModelPath::StepField::Fx2;
      return true;
    }
    return bad("the step field must be note, instr, vol, fx1 or fx2");
  }
  if (part[0] == "inst" && part.size() >= 3) {
    if (!parse_uint(part[1], 1, model::kMaxInstruments, n)) {
      return bad("the instrument must be 1..64");
    }
    out.a = static_cast<unsigned>(n);
    if (part.size() == 3 && part[2] == "name") {
      out.kind = ModelPath::Kind::InstName;
      return true;
    }
    const std::string caption = path.substr(part[0].size() + part[1].size() + 2u);
    for (const ParamName& p : param_names()) {
      if (p.caption == caption) {
        out.kind = ModelPath::Kind::InstParam;
        out.field = p.field;
        return true;
      }
    }
    return bad("no instrument parameter " + quoted(caption));
  }
  error = "unknown model path " + quoted(path) + " (tempo, name, order.<n>, order.len, pat.<p>.<t>.<s>.<field>, inst.<n>.name, inst.<n>.<param>)";
  return false;
}

struct Value {
  bool numeric = false;
  bool integral = false;  // a whole-number read (tempo, the song, a step's bytes, a MIDI channel, a parameter that is an Int or an Enum): compared exactly, no tolerance
  double num = 0.0;
  std::string text;
};

Value number_value(double v, bool integral = false) {
  Value out;
  out.numeric = true;
  out.integral = integral;
  out.num = v;
  out.text = number_text(v);
  return out;
}

// A value stored as a whole number in the row: its unit shows it whole (an Int, Pan, Enum, Program or CcSlot row of model/params.h).
bool whole_unit(core::ParamUnit unit) {
  return unit == core::ParamUnit::Int || unit == core::ParamUnit::Pan || unit == core::ParamUnit::Enum || unit == core::ParamUnit::Program || unit == core::ParamUnit::CcSlot;
}

bool read_value(const model::Project& project, const ModelPath& path, Value& out, std::string& why) {
  switch (path.kind) {
    case ModelPath::Kind::Tempo:
      out = number_value(static_cast<double>(project.tempo_x100()), true);
      return true;
    case ModelPath::Kind::Name:
      out.text.assign(project.name(), ::strnlen(project.name(), model::kProjectNameMax));
      return true;
    case ModelPath::Kind::OrderLen:
      out = number_value(static_cast<double>(project.song_length()), true);
      return true;
    case ModelPath::Kind::Order:
      out = number_value(static_cast<double>(project.order(path.a)), true);
      return true;
    case ModelPath::Kind::Step: {
      const model::Pattern* pattern = project.pattern(path.a);
      if (pattern == nullptr) {
        why = "the document has no pattern " + std::to_string(path.a);
        return false;
      }
      const model::Step& s = pattern->steps[path.track][path.step];
      switch (path.step_field) {
        case ModelPath::StepField::Note: out = number_value(s.note, true); return true;
        case ModelPath::StepField::Instr: out = number_value(s.instr, true); return true;
        case ModelPath::StepField::Vol: out = number_value(s.vol, true); return true;
        case ModelPath::StepField::Fx1:
        case ModelPath::StepField::Fx2: {
          const model::StepFx& fx = s.fx[path.step_field == ModelPath::StepField::Fx1 ? 0 : 1];
          out.text = std::to_string(fx.type) + ":" + std::to_string(fx.value);
          return true;
        }
      }
      return false;
    }
    case ModelPath::Kind::InstName: {
      const model::Instrument* in = project.instrument(path.a - 1u);
      if (in == nullptr) {
        why = "instrument " + std::to_string(path.a) + " is empty";
        return false;
      }
      out.text.assign(in->name, ::strnlen(in->name, sizeof in->name));
      return true;
    }
    case ModelPath::Kind::InstParam: {
      const model::Instrument* in = project.instrument(path.a - 1u);
      if (in == nullptr) {
        why = "instrument " + std::to_string(path.a) + " is empty";
        return false;
      }
      if (path.field == model::instrument_param::MidiChannel) {
        out = number_value(in->midi_channel, true);
        return true;
      }
      if (path.field == model::instrument_param::PlayStereo) {
        out = number_value(in->stereo, true);
        return true;
      }
      const core::ParamId id = core::param_id(core::ParamKind::Instrument, path.a - 1u, path.field);
      if (!project.has_param(id)) {
        why = "instrument " + std::to_string(path.a) + " has no such parameter (its kind is not a sample instrument's?)";
        return false;
      }
      const core::ParamDesc* desc = model::param_desc(id);
      out = number_value(static_cast<double>(project.get_param(id)), desc != nullptr && whole_unit(desc->unit));
      return true;
    }
  }
  return false;
}

// A float parameter has 1e-4 of tolerance, relative above 1: it is not stored with more digits than that. A whole-number read (Value::integral) has none: 13001 is not 13000.
bool same_number(double have, double want) { return std::fabs(have - want) <= 1e-4 * std::fmax(1.0, std::fabs(want)); }

// `want` as the whole number it spells (an optional minus and decimal digits, nothing else); false for "13000.0", "1e4", "+5" or " 5": a whole-number read is wanted as it prints.
bool whole_number(const std::string& want, double& out) {
  std::size_t i = want.size() > 1u && want[0] == '-' ? 1u : 0u;
  if (i == want.size() || want.size() - i > 15u) {
    return false;
  }
  double v = 0.0;
  for (; i < want.size(); ++i) {
    if (want[i] < '0' || want[i] > '9') {
      return false;
    }
    v = v * 10.0 + static_cast<double>(want[i] - '0');
  }
  out = want[0] == '-' ? -v : v;
  return true;
}

class HashSink final : public core::ByteSink {
 public:
  core::IoResult write(const void* src, std::size_t n) noexcept override {
    sha.update(src, n);
    return {n, core::IoStatus::Ok};
  }
  platform::host::Sha256 sha;
};

std::string hex_of(const std::uint8_t (&digest)[32]) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  for (const std::uint8_t b : digest) {
    out += kDigits[b >> 4];
    out += kDigits[b & 15u];
  }
  return out;
}

}  // namespace

// ---- assert state --------------------------------------------------------------------------------------------------------------------

bool parse_state_pairs(const std::vector<std::string>& words, std::vector<StatePair>& out, std::string& error) {
  out.clear();
  if (words.empty()) {
    error = "assert state needs at least one key=value";
    return false;
  }
  for (const std::string& word : words) {
    const std::size_t eq = word.find('=');
    if (eq == std::string::npos || eq == 0) {
      error = "assert state wants key=value, not " + quoted(word);
      return false;
    }
    StatePair pair{word.substr(0, eq), word.substr(eq + 1)};
    if (pair.value == "~") {  // (an empty expression matches every value: an assertion that cannot fail)
      error = "assert state: " + quoted(pair.key + "=~") + " has no regular expression after the ~ (an empty one matches anything)";
      return false;
    }
    if (!pair.value.empty() && pair.value[0] == '~' && !Regex(pair.value.substr(1)).ok()) {
      error = "assert state: " + quoted(pair.value.substr(1)) + " is not a regular expression (POSIX extended)";
      return false;
    }
    out.push_back(std::move(pair));
  }
  return true;
}

std::string state_problem(const std::string& line, const std::vector<StatePair>& pairs) {
  const std::map<std::string, std::string> have = parse_pairs(line);
  for (const StatePair& pair : pairs) {
    const auto it = have.find(pair.key);
    const std::string actual = it == have.end() ? std::string() : it->second;
    const bool regex = !pair.value.empty() && pair.value[0] == '~';
    const bool holds = regex ? Regex(pair.value.substr(1)).search(actual) : (it != have.end() && actual == pair.value);
    if (!holds) {
      return it == have.end() ? "the state line has no " + pair.key + " (" + line + ")"
                              : "state " + pair.key + " is " + quoted(actual) + ", wanted " + (regex ? "~" : "") + quoted(regex ? pair.value.substr(1) : pair.value);
    }
  }
  return std::string();
}

// ---- assert model --------------------------------------------------------------------------------------------------------------------

bool is_sha256_hex(const std::string& text) {
  if (text.size() != 64u) {
    return false;
  }
  for (const char c : text) {
    if (!std::isxdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  return true;
}

bool split_model_args(const std::vector<std::string>& words, std::string& path, std::string& value) {
  if (words.size() != 1u) {
    return false;
  }
  const std::size_t eq = words[0].find('=');
  if (eq == std::string::npos || eq == 0 || eq + 1u == words[0].size()) {
    return false;
  }
  path = words[0].substr(0, eq);
  value = words[0].substr(eq + 1u);
  return true;
}

bool valid_model_path(const std::string& path, std::string& error) {
  ModelPath parsed;
  return parse_model_path(path, parsed, error);
}

bool model_read(const model::Project& project, const std::string& path, std::string& text, std::string& why) {
  ModelPath parsed;
  Value value;
  if (!parse_model_path(path, parsed, why) || !read_value(project, parsed, value, why)) {
    return false;
  }
  text = value.text;
  return true;
}

std::string model_problem(const model::Project& project, const std::string& path, const std::string& want) {
  ModelPath parsed;
  Value value;
  std::string why;
  if (!parse_model_path(path, parsed, why) || !read_value(project, parsed, value, why)) {
    return why;
  }
  bool holds = value.text == want;
  if (value.numeric && value.integral) {
    double wanted = 0.0;
    holds = whole_number(want, wanted) && value.num == wanted;  // exactly: 13001 is not 13000
  } else if (value.numeric) {
    char* end = nullptr;
    errno = 0;
    const double wanted = std::strtod(want.c_str(), &end);
    if (!want.empty() && *end == '\0' && errno == 0 && std::isfinite(wanted)) {
      holds = same_number(value.num, wanted);
    }
  }
  if (holds) {
    return std::string();
  }
  return "model " + path + " is " + quoted(value.text) + ", wanted " + quoted(want);
}

const std::vector<std::string>& instrument_param_captions() {
  static const std::vector<std::string> captions = [] {
    std::vector<std::string> out;
    for (const ParamName& p : param_names()) {
      out.push_back(p.caption);
    }
    return out;
  }();
  return captions;
}

namespace {

// QANET B5: the document walked field by field (the values the model holds, never the bytes of a struct: a padding byte or the tail of a name after its NUL is no state). The digest has this stream after the
// native writer's: a field the writer drops reads back as another value, which the writer's own bytes cannot show (they are the same before and after a save that loses the field).
class FieldFeed {
 public:
  explicit FieldFeed(platform::host::Sha256& sha) : sha_(sha) {}
  template <typename T>
  void value(const T& v) {
    sha_.update(&v, sizeof v);  // (an arithmetic object: no padding)
  }
  void text(const char* s, std::size_t cap) {
    std::size_t n = 0;
    while (n < cap && s[n] != '\0') {
      ++n;
    }
    value(static_cast<std::uint32_t>(n));
    sha_.update(s, n);
  }

 private:
  platform::host::Sha256& sha_;
};

void feed_sampler(FieldFeed& f, const model::SamplerParams& s) {
  f.value(s.cutoff);
  f.value(s.resonance);
  for (const model::Envelope& e : s.envelope) {
    f.value(e.amount);
    f.value(e.delay);
    f.value(e.attack);
    f.value(e.hold);
    f.value(e.decay);
    f.value(e.sustain);
    f.value(e.release);
    f.value(e.loop);
    f.value(e.enable);
  }
  for (const model::Lfo& l : s.lfo) {
    f.value(l.shape);
    f.value(l.speed);
    f.value(l.amount);
  }
  f.value(s.wavetable_window);
  f.value(s.start);
  f.value(s.loop_start);
  f.value(s.loop_end);
  f.value(s.end);
  for (const std::uint16_t v : s.slices) {
    f.value(v);
  }
  f.value(s.glide);
  f.value(s.wavetable_window_size);
  f.value(s.grain_length);
  f.value(s.grain_position);
  f.value(s.sample_type);
  f.value(s.play_mode);
  f.value(s.filter_type);
  f.value(s.filter_enable);
  f.value(s.tune);
  f.value(s.fine_tune);
  f.value(s.volume);
  f.value(s.panning);
  f.value(s.delay_send);
  f.value(s.reverb_send);
  f.value(s.overdrive);
  f.value(s.bit_depth);
  f.value(s.slice_count);
  f.value(s.selected_slice);
  f.value(s.grain_shape);
  f.value(s.grain_type);
}

void feed_fields(platform::host::Sha256& sha, const model::Project& p) {
  FieldFeed f(sha);
  f.text(p.name(), model::kProjectNameMax);
  f.value(p.tempo_x100());
  f.value(p.swing());
  f.value(p.default_pattern_length());
  f.value(p.active_tracks());
  const model::MasterSettings& m = p.master();
  f.value(m.volume);
  f.value(m.dry_volume);
  f.value(m.reverb_volume);
  f.value(m.delay_volume);
  f.value(m.flags);
  for (std::uint32_t t = 0; t < model::kMaxTracks; ++t) {
    const model::TrackSlot* const k = p.track(t);
    f.value(static_cast<std::uint8_t>(k->type));
    f.value(k->flags);
    f.value(k->volume);
    f.value(k->pan);
    f.value(k->bus);
    f.text(k->name, model::kTrackNameMax);
  }
  f.value(p.song_length());
  for (std::uint32_t i = 0; i < p.song_length(); ++i) {
    f.value(p.order(i));
  }
  f.value(p.pattern_count());
  for (std::uint32_t id = 1; id <= 255u; ++id) {
    const model::Pattern* const pat = p.pattern(id);
    if (pat == nullptr) {
      continue;
    }
    f.value(id);
    for (const std::uint8_t len : pat->track_length) {
      f.value(len);
    }
    f.text(pat->name, model::kPatternNameMax);
    sha.update(pat->steps, sizeof pat->steps);  // (Step has no padding: the static_assert of model/pattern.h)
  }
  for (std::uint32_t i = 0; i < model::kMaxInstruments; ++i) {
    const model::Instrument* const in = p.instrument(i);
    f.value(static_cast<std::uint8_t>(in != nullptr ? 1 : 0));
    if (in == nullptr) {
      continue;
    }
    f.value(static_cast<std::uint8_t>(in->kind));
    f.value(in->midi_channel);
    f.value(in->module_index);
    f.value(in->engine);
    f.value(in->param_len);
    f.text(in->name, model::kInstrumentNameMax);
    f.text(in->sample_path, model::kSamplePathMax);
    f.value(in->sample_rate);
    feed_sampler(f, in->sampler);
    sha.update(in->params, in->param_len < model::kModuleParamBytes ? in->param_len : model::kModuleParamBytes);
    f.value(in->midi_velocity);
    f.value(in->midi_program);
    f.value(in->midi_out);
    f.value(in->midi_pb_range);
    f.value(in->midi_offset);
    for (const std::uint8_t cc : in->midi_cc) {
      f.value(cc);
    }
    f.value(in->vel_sens);
  }
}

}  // namespace

std::string model_digest(const model::Project& project) {
  auto writer = std::make_unique<formats::native::Writer>();  // (1.4 KB of scratch: off the stack)
  HashSink sink;
  writer->begin(project, sink);
  formats::Progress progress = formats::Progress::Running;
  while (progress == formats::Progress::Running) {
    progress = writer->step(formats::native::kDefaultStepBudget);
  }
  if (progress != formats::Progress::Done) {
    return std::string();
  }
  feed_fields(sink.sha, project);
  std::uint8_t digest[32];
  sink.sha.finish(digest);
  return hex_of(digest);
}

// ---- assert file ---------------------------------------------------------------------------------------------------------------------

bool parse_file_want(const std::vector<std::string>& words, std::string& path, FileWant& want, std::string& error) {
  if (words.size() != 2u) {
    error = "assert file needs a path (in quotes when it has a space) and one test: exists, absent, size=<n>, size><n>, size<<n>, sha=<64 hex digits>";
    return false;
  }
  if (!hal::storage::valid_path(words[0].c_str())) {
    error = "assert file: " + quoted(words[0]) + " is not a path of the card";
    return false;
  }
  path = words[0];
  const std::string test = words[1];
  unsigned long n = 0;
  if (test == "exists" || test == "absent") {
    want.test = test == "exists" ? FileWant::Test::Exists : FileWant::Test::Absent;
    return true;
  }
  if (test.compare(0, 5, "size=") == 0 || test.compare(0, 5, "size>") == 0 || test.compare(0, 5, "size<") == 0) {
    if (!parse_uint(test.substr(5), 0, 0xFFFFFFFFul, n)) {
      error = "assert file: the size must be a whole number of bytes, 0..4294967295";
      return false;
    }
    want.test = test[4] == '=' ? FileWant::Test::SizeEq : (test[4] == '>' ? FileWant::Test::SizeGt : FileWant::Test::SizeLt);
    want.size = n;
    return true;
  }
  if (test.compare(0, 4, "sha=") == 0 && is_sha256_hex(test.substr(4))) {
    want.test = FileWant::Test::Sha;
    want.sha = lower(test.substr(4));
    return true;
  }
  error = "assert file: " + quoted(test) + " is not a test (exists, absent, size=<n>, size><n>, size<<n>, sha=<64 hex digits>)";
  return false;
}

std::string file_problem(hal::storage::Storage* card, const std::string& path, const FileWant& want) {
  using hal::storage::Status;
  if (card == nullptr) {
    return "no card";
  }
  if (!card->card_present()) {
    return "the card is pulled";
  }
  if (!card->mounted()) {
    return "the card is not mounted";
  }
  bool is_dir = false;
  const Status found = card->exists(path.c_str(), &is_dir);
  if (want.test == FileWant::Test::Absent) {
    if (found == Status::Ok) {
      return path + " exists";
    }
    return found == Status::NotFound || found == Status::NotADirectory ? std::string() : path + ": " + hal::storage::status_name(found);
  }
  if (found != Status::Ok) {
    return path + " is not on the card (" + hal::storage::status_name(found) + ")";
  }
  if (want.test == FileWant::Test::Exists) {
    return std::string();
  }
  if (is_dir) {
    return path + " is a folder";
  }
  hal::storage::FileHandle file;
  Status s = card->open(path.c_str(), file);
  if (s != Status::Ok) {
    return path + " cannot be opened (" + hal::storage::status_name(s) + ")";
  }
  std::uint32_t size = 0;
  s = card->size(file, size);
  if (s != Status::Ok) {
    card->close(file);
    return path + ": " + hal::storage::status_name(s);
  }
  if (want.test != FileWant::Test::Sha) {
    card->close(file);
    const bool holds = want.test == FileWant::Test::SizeEq ? size == want.size : (want.test == FileWant::Test::SizeGt ? size > want.size : size < want.size);
    const char* relation = want.test == FileWant::Test::SizeEq ? "= " : (want.test == FileWant::Test::SizeGt ? "> " : "< ");
    return holds ? std::string() : path + " is " + std::to_string(size) + " bytes, wanted " + relation + std::to_string(want.size);
  }
  platform::host::Sha256 sha;
  std::uint8_t buf[4096];
  for (;;) {
    std::size_t got = 0;
    s = card->read(file, buf, sizeof buf, got);
    if (s != Status::Ok) {
      card->close(file);
      return path + " cannot be read (" + hal::storage::status_name(s) + ")";
    }
    if (got == 0) {
      break;
    }
    sha.update(buf, got);
  }
  card->close(file);
  std::uint8_t digest[32];
  sha.finish(digest);
  const std::string have = hex_of(digest);
  return have == want.sha ? std::string() : path + " has the SHA-256 " + have + ", wanted " + want.sha;
}

}  // namespace o3os::sim
