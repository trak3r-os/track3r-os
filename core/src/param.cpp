// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/core/param.h>

#include <o3os/core/fast_math.h>

namespace o3os::core {

namespace {

constexpr float kExpShape = 4.0f;  // Exp: v = min + (max - min) (2^(4 n) - 1) / 15
constexpr float kExpDenominator = 15.0f;

float clamp01(float n) noexcept { return n > 0.0f ? (n < 1.0f ? n : 1.0f) : 0.0f; }  // NaN gives 0

bool is_nan(float v) noexcept { return v != v; }

// Nearest whole number, halves away from zero; NaN is 0 and the result is held to +-1e9 so the conversion is defined.
long round_l(float v) noexcept {
  if (is_nan(v)) {
    return 0;
  }
  v = v > 1.0e9f ? 1.0e9f : (v < -1.0e9f ? -1.0e9f : v);
  return static_cast<long>(v + (v < 0.0f ? -0.5f : 0.5f));
}

char lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c; }
bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

// Characters into a buffer of `cap` bytes, always NUL terminated, cut when it does not fit.
struct Out {
  char* p;
  unsigned cap;
  unsigned n = 0;

  void ch(char c) noexcept {
    if (n + 1 < cap) {
      p[n++] = c;
    }
  }
  void str(const char* s) noexcept {
    while (*s != '\0') {
      ch(*s++);
    }
  }
  void uint(unsigned long v) noexcept {
    char t[12];
    unsigned k = 0;
    do {
      t[k++] = static_cast<char>('0' + v % 10ul);
      v /= 10ul;
    } while (v != 0 && k < sizeof t);
    while (k != 0) {
      ch(t[--k]);
    }
  }
  void sint(long v) noexcept {
    if (v < 0) {
      ch('-');
      uint(0ul - static_cast<unsigned long>(v));
    } else {
      uint(static_cast<unsigned long>(v));
    }
  }
  unsigned finish() noexcept {
    if (cap != 0) {
      p[n] = '\0';
    }
    return n;
  }
};

// The names of an Enum: as many as it says it has (param_desc_valid checks that this is one for each value, max - min + 1: the first name is the minimum, which a module's labelled INT may set above 0).
unsigned name_count(const ParamDesc& d) noexcept { return d.list_count != 0 ? d.list_count : 1u; }

const char* enum_name(const ParamDesc& d, float value) noexcept {
  const long i = round_l(value - d.min);
  const long last = static_cast<long>(name_count(d)) - 1;
  return d.names[i < 0 ? 0 : (i > last ? last : i)];
}

// ---- parsing ----------------------------------------------------------------------------------------------------

void skip_spaces(const char*& s) noexcept {
  while (*s == ' ') {
    ++s;
  }
}

// [sign] digits [. digits]: at most 9 integer digits, six decimals are used and the rest ignored. False when there is no digit.
bool parse_decimal(const char*& s, float& out) noexcept {
  bool neg = false;
  if (*s == '-' || *s == '+') {
    neg = *s == '-';
    ++s;
  }
  unsigned long whole = 0;
  unsigned long frac = 0;
  unsigned long scale = 1;
  unsigned digits = 0;
  unsigned decimals = 0;
  bool any = false;
  while (is_digit(*s)) {
    if (digits >= 9) {
      return false;
    }
    whole = whole * 10ul + static_cast<unsigned long>(*s - '0');
    ++digits;
    any = true;
    ++s;
  }
  if (*s == '.') {
    ++s;
    while (is_digit(*s)) {
      if (decimals < 6) {
        frac = frac * 10ul + static_cast<unsigned long>(*s - '0');
        scale *= 10ul;
        ++decimals;
      }
      any = true;
      ++s;
    }
  }
  if (!any) {
    return false;
  }
  const float v = static_cast<float>(whole) + static_cast<float>(frac) / static_cast<float>(scale);
  out = neg ? -v : v;
  return true;
}

// Only the unit text (case-insensitive) may follow, then the end.
bool tail_ok(const char* s, const char* unit_text) noexcept {
  skip_spaces(s);
  if (*s == '\0') {
    return true;
  }
  if (unit_text[0] == '\0') {
    return false;
  }
  std::size_t i = 0;
  while (unit_text[i] != '\0' && lower(s[i]) == lower(unit_text[i])) {
    ++i;
  }
  if (unit_text[i] != '\0') {
    return false;
  }
  s += i;
  skip_spaces(s);
  return *s == '\0';
}

}  // namespace

bool param_desc_valid(const ParamDesc& d) noexcept {
  if (d.name == nullptr || d.name[0] == '\0' || d.unit_text == nullptr) {
    return false;
  }
  unsigned n = 0;
  while (d.name[n] != '\0') {
    ++n;
  }
  if (n > kParamNameChars) {
    return false;
  }
  if (!(d.max > d.min) || is_nan(d.def) || d.def < d.min || d.def > d.max || d.detents < 1 || d.coarse < 1) {
    return false;
  }
  if (d.unit == ParamUnit::Enum) {
    if (d.names == nullptr || static_cast<float>(d.list_count) != d.max - d.min + 1.0f) {
      return false;
    }
    for (unsigned i = 0; i < d.list_count; ++i) {
      if (d.names[i] == nullptr || d.names[i][0] == '\0') {
        return false;
      }
    }
  }
  if (d.curve == ParamCurve::Log && !(d.min > 0.0f)) {
    return false;
  }
  if (d.curve == ParamCurve::Table) {
    if (d.knots == nullptr || d.list_count < 2 || d.knots[0].n != 0.0f || d.knots[0].v != d.min ||
        d.knots[d.list_count - 1u].n != 1.0f || d.knots[d.list_count - 1u].v != d.max) {
      return false;
    }
    for (unsigned i = 1; i < d.list_count; ++i) {
      if (!(d.knots[i].n > d.knots[i - 1u].n) || !(d.knots[i].v > d.knots[i - 1u].v)) {
        return false;
      }
    }
  }
  return true;
}

bool param_in_range(const ParamDesc& d, float value) noexcept { return value >= d.min && value <= d.max; }  // false for NaN

float param_clamp(const ParamDesc& d, float value) noexcept {
  if (is_nan(value)) {
    return d.def;
  }
  return value < d.min ? d.min : (value > d.max ? d.max : value);
}

float param_norm(const ParamDesc& d, float value) noexcept {
  if (!(d.max > d.min) || !(value > d.min)) {
    return 0.0f;  // also NaN
  }
  if (value >= d.max) {
    return 1.0f;
  }
  switch (d.curve) {
    case ParamCurve::Log:
      if (d.min > 0.0f) {
        return clamp01(log2_fast(value / d.min) / log2_fast(d.max / d.min));
      }
      break;
    case ParamCurve::Exp:
      return clamp01(log2_fast(1.0f + kExpDenominator * (value - d.min) / (d.max - d.min)) / kExpShape);
    case ParamCurve::Table:
      for (unsigned i = 1; i < d.list_count; ++i) {
        if (value <= d.knots[i].v) {
          const ParamKnot& a = d.knots[i - 1u];
          const ParamKnot& b = d.knots[i];
          return clamp01(a.n + (value - a.v) / (b.v - a.v) * (b.n - a.n));
        }
      }
      return 1.0f;
    case ParamCurve::Linear:
      break;
  }
  return clamp01((value - d.min) / (d.max - d.min));
}

float param_denorm(const ParamDesc& d, float norm) noexcept {
  const float n = clamp01(norm);
  if (n <= 0.0f) {
    return d.min;  // the ends are exact whatever the curve's rounding
  }
  if (n >= 1.0f) {
    return d.max;
  }
  float v;
  switch (d.curve) {
    case ParamCurve::Log:
      v = d.min > 0.0f ? d.min * exp2_fast(n * log2_fast(d.max / d.min)) : d.min + n * (d.max - d.min);
      break;
    case ParamCurve::Exp:
      v = d.min + (d.max - d.min) * (exp2_fast(kExpShape * n) - 1.0f) / kExpDenominator;
      break;
    case ParamCurve::Table:
      v = d.max;
      for (unsigned i = 1; i < d.list_count; ++i) {
        if (n <= d.knots[i].n) {
          const ParamKnot& a = d.knots[i - 1u];
          const ParamKnot& b = d.knots[i];
          v = a.v + (n - a.n) / (b.n - a.n) * (b.v - a.v);
          break;
        }
      }
      break;
    case ParamCurve::Linear:
    default:
      v = d.min + n * (d.max - d.min);
      break;
  }
  return v < d.min ? d.min : (v > d.max ? d.max : v);
}

float param_step(const ParamDesc& d, float value, int detents) noexcept {
  const float count = static_cast<float>(d.detents != 0 ? d.detents : 1);
  if (d.curve == ParamCurve::Linear) {
    // Added to the value as it is, like the cutoff step of 0.01 (whole numbers stay whole, an off-grid value keeps its offset).
    return param_clamp(d, param_clamp(d, value) + static_cast<float>(detents) * ((d.max - d.min) / count));
  }
  return param_denorm(d, param_norm(d, value) + static_cast<float>(detents) / count);
}

unsigned param_format(const ParamDesc& d, float value, char* out, unsigned cap) noexcept {
  Out o{out, cap};
  switch (d.unit) {
    case ParamUnit::Pan:
      o.sint(round_l(value - (d.min + d.max) * 0.5f));
      break;
    case ParamUnit::NormPct:
      o.sint(round_l(param_norm(d, value) * 100.0f));
      break;
    case ParamUnit::Seconds: {
      const long ms = round_l(value) < 0 ? 0 : round_l(value);
      o.uint(static_cast<unsigned long>(ms / 1000));
      o.ch('.');
      const unsigned long frac = static_cast<unsigned long>(ms % 1000);
      o.ch(static_cast<char>('0' + frac / 100ul));
      o.ch(static_cast<char>('0' + (frac / 10ul) % 10ul));
      o.ch(static_cast<char>('0' + frac % 10ul));
      break;
    }
    case ParamUnit::Enum:
      if (d.names != nullptr) {
        o.str(enum_name(d, value));
        break;
      }
      o.sint(round_l(value));
      break;
    case ParamUnit::Program: {
      const long n = round_l(value);
      if (n <= 0) {
        o.str("NONE");
      } else {
        o.sint(n - 1);
      }
      break;
    }
    case ParamUnit::CcSlot: {
      const long n = round_l(value);
      if (n <= 0) {
        o.str("CONFIG");
      } else if (n <= 128) {
        o.str("CC ");
        o.sint(n - 1);
      } else {
        o.str(n == 129 ? "PROG" : (n == 130 ? "CH AT" : "POLY AT"));
      }
      break;
    }
    case ParamUnit::WtWave: {  // OS-CEWTENV W7a: 0..127 are F001..F128, 128..255 U001..U128
      const long n = round_l(value) < 0 ? 0 : round_l(value);
      o.ch(n < 128 ? 'F' : 'U');
      const unsigned long k = static_cast<unsigned long>(n & 127) + 1ul;
      o.ch(static_cast<char>('0' + k / 100ul));
      o.ch(static_cast<char>('0' + (k / 10ul) % 10ul));
      o.ch(static_cast<char>('0' + k % 10ul));
      break;
    }
    case ParamUnit::OffInt:  // 0 shows "OFF"
      if (round_l(value) <= 0) {
        o.str("OFF");
        break;
      }
      o.sint(round_l(value));
      break;
    case ParamUnit::Int:
    default:
      o.sint(round_l(value));
      break;
  }
  return o.finish();
}

bool param_parse(const ParamDesc& d, const char* text, float& out) noexcept {
  if (text == nullptr) {
    return false;
  }
  const char* s = text;
  skip_spaces(s);
  float v = 0.0f;
  if (d.unit == ParamUnit::Enum) {
    if (d.names == nullptr) {
      return false;
    }
    // The whole text, trailing spaces ignored, is one name.
    std::size_t len = 0;
    while (s[len] != '\0') {
      ++len;
    }
    while (len != 0 && s[len - 1] == ' ') {
      --len;
    }
    for (unsigned i = 0; i < name_count(d); ++i) {
      std::size_t k = 0;
      while (k < len && d.names[i][k] != '\0' && lower(d.names[i][k]) == lower(s[k])) {
        ++k;
      }
      if (k == len && d.names[i][k] == '\0') {
        v = d.min + static_cast<float>(i);
        out = v;
        return true;
      }
    }
    return false;
  }
  if (d.unit == ParamUnit::Program || d.unit == ParamUnit::CcSlot) {  // OS-INSTUI S8b: the words, else the number (a CC after an optional "CC")
    const auto word = [&s](const char* w) noexcept {
      std::size_t k = 0;
      while (w[k] != '\0' && lower(s[k]) == lower(w[k])) {
        ++k;
      }
      if (w[k] != '\0') {
        return false;
      }
      const char* t = s + k;
      skip_spaces(t);
      return *t == '\0';
    };
    if (d.unit == ParamUnit::Program ? word("NONE") : word("CONFIG")) {
      out = 0.0f;
      return true;
    }
    if (d.unit == ParamUnit::CcSlot) {
      const float special[3] = {129.0f, 130.0f, 131.0f};
      const char* const words[3] = {"PROG", "CH AT", "POLY AT"};
      for (unsigned i = 0; i < 3; ++i) {
        if (word(words[i])) {
          out = special[i];
          return true;
        }
      }
      if (lower(s[0]) == 'c' && lower(s[1]) == 'c') {
        s += 2;
        skip_spaces(s);
      }
    }
    float n = 0.0f;
    if (!parse_decimal(s, n) || !tail_ok(s, "") || n < 0.0f || n > 127.0f || n != static_cast<float>(static_cast<long>(n))) {  // (a program or a controller: 0..127)
      return false;
    }
    v = n + 1.0f;
    if (v < d.min || v > d.max) {
      return false;
    }
    out = v;
    return true;
  }
  float number = 0.0f;
  if (!parse_decimal(s, number) || !tail_ok(s, d.unit_text)) {
    return false;
  }
  switch (d.unit) {
    case ParamUnit::Pan:
      v = number + (d.min + d.max) * 0.5f;
      break;
    case ParamUnit::NormPct:
      v = param_denorm(d, number * 0.01f);
      break;
    case ParamUnit::Seconds:
      v = static_cast<float>(round_l(number * 1000.0f));
      break;
    default:
      v = number;
      break;
  }
  if (is_nan(v) || v < d.min || v > d.max) {
    return false;
  }
  out = v;
  return true;
}

}  // namespace o3os::core
