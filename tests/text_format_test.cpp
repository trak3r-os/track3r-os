// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-FLASHDIET P2: append_uint / append_dec / append_fx_value are forwarders over one out-of-line writer each, whatever the string's capacity. Whatever the capacity,
// the text is the one a reference makes and a short string holds its prefix (the cut FixedString makes).
#include <doctest/doctest.h>

#include <o3os/app/fx_text.h>
#include <o3os/app/text_format.h>
#include <o3os/core/fixed_string.h>

#include <cstdint>
#include <string>

namespace {

using namespace o3os;

template <std::size_t N>
std::string uint_text(unsigned value, const char* lead) {
  core::FixedString<N> s;
  s.assign(lead);
  app::append_uint(s, value);
  return s.c_str();
}

template <std::size_t N>
std::string dec_text(unsigned value, unsigned digits) {
  core::FixedString<N> s;
  app::append_dec(s, value, digits);
  return s.c_str();
}

template <std::size_t N>
void check_cut(const std::string& full, std::uint8_t type, std::uint8_t value) {
  core::FixedString<N> s;
  CHECK(app::append_fx_value(s, type, value));
  CHECK(std::string(s.c_str()) == full.substr(0, N));
  CHECK(s.size() == std::string(s.c_str()).size());
}

}  // namespace

TEST_CASE("text_format: append_uint and append_dec give the reference digits at any capacity, and cut like FixedString") {
  const unsigned values[] = {0u, 1u, 9u, 10u, 99u, 100u, 12345u, 4294967295u};
  for (unsigned v : values) {
    CAPTURE(v);
    const std::string d = std::to_string(v);
    CHECK(uint_text<24>(v, "N ") == "N " + d);
    CHECK(uint_text<40>(v, "") == d);
    CHECK(uint_text<4>(v, "AB") == ("AB" + d).substr(0, 4));
    CHECK(uint_text<2>(v, "AB") == "AB");  // a full string takes nothing
    for (unsigned digits = 1; digits <= 10; ++digits) {
      std::string want = d;
      if (want.size() > digits) {
        want = want.substr(want.size() - digits);  // a larger value keeps its low digits
      } else {
        want = std::string(digits - want.size(), '0') + want;
      }
      CAPTURE(digits);
      CHECK(dec_text<24>(v, digits) == want);
      CHECK(dec_text<3>(v, digits) == want.substr(0, 3));
    }
  }
}

TEST_CASE("text_format: append_fx_value says the same words at any capacity, and a short string holds the prefix") {
  for (unsigned type = 0; type < 256u; ++type) {
    for (unsigned value = 0; value < 256u; ++value) {
      core::FixedString<40> full;
      const bool said = app::append_fx_value(full, static_cast<std::uint8_t>(type), static_cast<std::uint8_t>(value));
      CAPTURE(type);
      CAPTURE(value);
      core::FixedString<24> mid;
      CHECK(app::append_fx_value(mid, static_cast<std::uint8_t>(type), static_cast<std::uint8_t>(value)) == said);
      if (!said) {
        CHECK(mid.empty());
        CHECK(full.empty());
        continue;
      }
      CHECK(std::string(mid.c_str()) == std::string(full.c_str()).substr(0, 24));
      check_cut<5>(full.c_str(), static_cast<std::uint8_t>(type), static_cast<std::uint8_t>(value));
      check_cut<1>(full.c_str(), static_cast<std::uint8_t>(type), static_cast<std::uint8_t>(value));
    }
  }
}
