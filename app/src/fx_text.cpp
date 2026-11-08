// SPDX-License-Identifier: GPL-3.0-or-later

// The one body of append_fx_value() (app/fx_text.h; OS-FLASHDIET P2): it was a template over the string's capacity, so every capacity that called it
// linked its own 1.5 KB copy (the Pattern screen's two and the Fill dialog's). It writes through the core's out-of-line appends into the caller's buffer,
// so the text is cut at the capacity exactly as a FixedString<N> cuts it.

#include <o3os/app/fx_text.h>
#include <o3os/core/inflate.h>
#include <o3os/model/fx_table.h>
#include <o3os/ui/assets.h>

namespace o3os::app {
namespace {

// A FixedString<N>'s storage seen without N: the same appends the FixedString makes, so the same cut.
struct Out {
  char* buf;
  std::size_t cap;
  std::size_t& len;
  bool append(const char* s) noexcept { return core::detail::string_append_z(buf, cap, len, s); }
  bool append(const char* s, std::size_t n) noexcept { return core::detail::string_append_n(buf, cap, len, s, n); }
};
bool append_dec(Out& s, unsigned value, unsigned digits) noexcept { return detail::append_dec(s.buf, s.cap, s.len, value, digits); }
bool append_uint(Out& s, unsigned value) noexcept { return detail::append_uint(s.buf, s.cap, s.len, value); }

bool fx_words(Out& s, std::uint8_t type, std::uint8_t value) noexcept {
  switch (type) {
    case seq::fx::kChance: {
      // OS-CECHANCE C2: the stored value is read as the sequencer reads it (clamped to 174, ADR 0004 amendment 4).
      const std::uint8_t v = value > seq::fx::kChanceMax ? seq::fx::kChanceMax : value;
      s.append("CHANCE ");
      if (v <= seq::fx::kChanceProbMax) {
        append_uint(s, v);
        s.append(" %");
      } else if (v == seq::fx::kChanceWithLast) {
        s.append("WITH LAST");
      } else if (v == seq::fx::kChanceNotLast) {
        s.append("NOT LAST");
      } else {
        std::uint32_t n = 0, m = 0;
        seq::fx::chance_cycle(v, n, m);  // 103..138 Play N / Skip M, 139..174 Skip N / Play M
        const bool skip_first = v >= 139u;
        s.append(skip_first ? "SKIP " : "PLAY ");
        append_uint(s, n);
        s.append(skip_first ? " PLAY " : " SKIP ");
        append_uint(s, m);
      }
      return true;
    }
    case seq::fx::kChord: {
      const std::uint8_t v = value < seq::kChordCount ? value : static_cast<std::uint8_t>(seq::kChordCount - 1u);  // above the range reads as the last: fx_sanitize holds the sequencer's reading there
      s.append("CHORD ");
      if (v == 0u) {
        s.append("OFF");
        return true;
      }
      const char* name = chord_name(v);
      if (name != nullptr) {
        s.append(name);
        s.append(" ");
      }
      for (unsigned b = 1; b < 15u; ++b) {  // one hex digit per offset above the root
        if (((seq::kChordMask[v] >> b) & 1u) != 0u) {
          s.append(&"0123456789ABCDEF"[b], 1);
        }
      }
      return true;
    }
    case seq::fx::kArp: {
      const std::uint8_t v = value > 33u ? std::uint8_t{33} : value;
      s.append("ARP ");
      if (v == 0u) {
        s.append("OFF");
        return true;
      }
      const seq::ArpDir dir = seq::arp_dir(v);
      const std::uint32_t rate = seq::arp_index(v) % seq::kArpRates;
      s.append(dir == seq::ArpDir::Up ? "UP " : (dir == seq::ArpDir::Down ? "DOWN " : "RND "));
      s.append(kArpRateNames[rate]);
      s.append(rate < 5u ? " STEPS" : " STEP");
      return true;
    }
    case seq::fx::kTune: {
      const int st = seq::tune_semitones(value);
      s.append("TUNE ");
      if (st > 0) {
        s.append("+");
      } else if (st < 0) {
        s.append("-");
      }
      append_uint(s, static_cast<unsigned>(st < 0 ? -st : st));
      s.append(" ST");
      return true;
    }
    case seq::fx::kSlideUp:
    case seq::fx::kSlideDown: {
      const unsigned hundredths = (static_cast<unsigned>(value) * 25u + 2u) / 4u;  // value / 16 semitones, in hundredths, rounded
      s.append(type == seq::fx::kSlideUp ? "SLIDE UP " : "SLIDE DOWN ");
      append_uint(s, hundredths / 100u);
      s.append(".");
      append_dec(s, hundredths % 100u, 2);
      s.append(" ST");
      return true;
    }
    case seq::fx::kGlide: {
      const std::uint32_t ms = audio::glide_fx_ms(value);
      s.append("GLIDE ");
      if (ms == 0u) {
        s.append("OFF");
      } else if (ms < 1000u) {
        append_uint(s, ms);
        s.append(" MS");
      } else {
        append_uint(s, ms / 1000u);
        s.append(".");
        append_dec(s, (ms % 1000u) / 10u, 2);
        s.append(" S");
      }
      return true;
    }
    case seq::fx::kOverdrive: {
      const unsigned v = value > 100u ? 100u : value;
      if (v == 0u) {
        s.append("DRIVE OFF");
      } else {
        s.append("DRIVE ");
        append_uint(s, v);
        s.append(" %");
      }
      return true;
    }
    case seq::fx::kBitDepth: {
      const unsigned v = value < 1u ? 1u : (value > 16u ? 16u : value);  // fx_sanitize holds the sequencer's reading to 1..16
      s.append("BITS ");
      append_uint(s, v);
      if (v == 16u) {
        s.append(" (OFF)");
      }
      return true;
    }
    case seq::fx::kTrkArp: {
      if (value == 0u) {
        s.append("ARPEG OFF");
        return true;
      }
      s.append("ARPEG +");
      append_uint(s, value >> 4);
      s.append(" +");
      append_uint(s, value & 15u);
      return true;
    }
    case seq::fx::kTrkVolSlide:
    case seq::fx::kTrkFineVol: {
      const unsigned up = value >> 4;    // the engine reads x first: x up, else y down (audio/tracker_fx.h)
      const unsigned down = value & 15u;
      s.append(type == seq::fx::kTrkVolSlide ? "VSLIDE " : "FVOL ");
      if (up != 0u) {
        s.append("+");
        append_uint(s, up);
      } else if (down != 0u) {
        s.append("-");
        append_uint(s, down);
      } else {
        s.append("0");
      }
      return true;
    }
    case seq::fx::kTrkFinePortaUp:
    case seq::fx::kTrkFinePortaDown: {
      const unsigned hundredths = static_cast<unsigned>(value) * 25u;  // value / 4 period units
      s.append(type == seq::fx::kTrkFinePortaUp ? "FINE UP " : "FINE DOWN ");
      append_uint(s, hundredths / 100u);
      s.append(".");
      append_dec(s, hundredths % 100u, 2);
      s.append(" PER");
      return true;
    }
    case seq::fx::kTrkPanSlide: {
      const unsigned x = value >> 4;  // the engine reads y (right) first, then x (left); Fy (y < F) and xF are the fine forms, once (audio/tracker_fx.h)
      const unsigned y = value & 15u;
      s.append("PANSL ");
      if (x == 15u && y != 0u && y != 15u) {
        s.append("FINE R ");
        append_uint(s, y);
      } else if (y == 15u && x != 0u) {
        s.append("FINE L ");
        append_uint(s, x);
      } else if (y != 0u) {
        s.append("R ");
        append_uint(s, y);
      } else if (x != 0u) {
        s.append("L ");
        append_uint(s, x);
      } else {
        s.append("0");
      }
      return true;
    }
    case seq::fx::kTrkVibWave:
    case seq::fx::kTrkTremWave: {
      static constexpr const char* kWave[4] = {"SINE", "RAMP", "SQUARE", "RANDOM"};
      s.append(type == seq::fx::kTrkVibWave ? "VIB " : "TREM ");
      s.append(kWave[value & 3u]);
      if ((value & 4u) != 0u) {
        s.append(" NORETRIG");
      }
      return true;
    }
    case seq::fx::kTrkGliss:
      s.append(value != 0u ? "GLISS ON" : "GLISS OFF");
      return true;
    case seq::fx::kTrkRetrig: {
      static constexpr const char* kVol[16] = {"", " VOL -1", " VOL -2", " VOL -4", " VOL -8", " VOL -16", " VOL X2/3", " VOL X1/2",
                                                "", " VOL +1", " VOL +2", " VOL +4", " VOL +8", " VOL +16", " VOL X3/2", " VOL X2"};
      s.append("RETRIG EVERY ");
      append_uint(s, value & 15u);
      s.append(kVol[value >> 4]);
      return true;
    }
    case seq::fx::kTrkChanPan:
      s.append("CHPAN ");
      append_uint(s, value > 100u ? 100u : value);
      return true;
    case seq::fx::kTrkChanVol:
    case seq::fx::kTrkGlobalVol:
      s.append(type == seq::fx::kTrkChanVol ? "CHVOL " : "GVOL ");
      append_uint(s, value > 64u ? 64u : value);
      return true;
    case seq::fx::kTrkChanVolSlide:
    case seq::fx::kTrkGlobalVolSlide: {
      const unsigned x = value >> 4;  // audio/tracker_fx.h: both read Fy / xF as the fine forms, once; plain, the channel's slide goes down first (y), the global one up first (x), as libopenmpt's
      const unsigned y = value & 15u;
      const bool chan = type == seq::fx::kTrkChanVolSlide;
      s.append(chan ? "CHVSL " : "GVSL ");
      if ((y == 15u && x != 0u) || (x == 15u && y != 0u)) {
        s.append("FINE ");
        s.append(y == 15u ? "+" : "-");
        append_uint(s, y == 15u ? x : y);
        return true;
      }
      const unsigned up = chan ? (y == 0u ? x : 0u) : x;
      const unsigned down = chan ? y : (x == 0u ? y : 0u);
      if (up != 0u) {
        s.append("+");
        append_uint(s, up);
      } else if (down != 0u) {
        s.append("-");
        append_uint(s, down);
      } else {
        s.append("0");
      }
      return true;
    }
    case seq::fx::kTrkPanbrello:
      s.append("PANBR SPD ");
      if ((value >> 4) != 0u) {
        append_uint(s, value >> 4);
      } else {
        s.append("KEEP");
      }
      s.append(" DEP ");
      if ((value & 15u) != 0u) {
        append_uint(s, value & 15u);
      } else {
        s.append("KEEP");
      }
      return true;
    case seq::fx::kAmpEnvFx: {  // OS-CEWTENV W6b: the WT slot's reading (duration scale x, warp offset y - 8 held to -7..7); on an ADSR amp slot the same nibbles are the attack and the decay
      const int warp = static_cast<int>(value & 15u) - 8;
      s.append("AMP ENV x");
      append_uint(s, value >> 4);
      s.append(" WARP ");
      s.append(warp < 0 ? "-" : "+");
      append_uint(s, static_cast<unsigned>((warp < 0 ? -warp : warp) > 7 ? 7 : (warp < 0 ? -warp : warp)));
      return true;
    }
    case seq::fx::kFilterEnvFx:  // the ADSR cutoff slot's reading: attack 8x ms (at least 1), decay 8y ms
      s.append("FLT ENV ");
      append_uint(s, (value >> 4) == 0u ? 1u : (value >> 4) * 8u);
      s.append("/");
      append_uint(s, (value & 15u) * 8u);
      s.append(" MS");
      return true;
    case seq::fx::kGranLenFx:  // F11's length in ms at 44.1 kHz: 0 is 1 ms, 50 is 100 ms, 100 is 1000 ms
      s.append("GRAIN ");
      append_uint(s, (audio::gran_len_fx_samples(value) * 10u + 220u) / 441u);  // rounded: 44 samples read 1 MS
      s.append(" MS");
      return true;
    case seq::fx::kTrkTremor:
      s.append("TREMOR ON ");
      append_uint(s, (value >> 4) + 1u);
      s.append(" OFF ");
      append_uint(s, (value & 15u) + 1u);
      return true;
    default:
      return false;
  }
}

}  // namespace

namespace detail {

bool fx_value_text(char* buf, std::size_t cap, std::size_t& len, std::uint8_t type, std::uint8_t value) noexcept {
  Out s{buf, cap, len};
  return fx_words(s, type, value);
}

}  // namespace detail

namespace {

// Line `line` of the packed picker text (fx_text.h: the names, then the descriptions) into out/cap, cut at cap. Decodes from the start of the stream through the UI's shared decoder, in chunks,
// and stops after the line's own newline; main loop only. Returns the length (0 with an empty string when the stream is unreadable or short).
unsigned fx_line(unsigned line, char* out, unsigned cap) noexcept {
  if (cap == 0u) {
    return 0u;
  }
  out[0] = '\0';
  core::Inflater& inf = ui::asset_inflater();
  inf.begin(kFxTextZ, kFxTextZSize, kFxTextRawSize);
  char buf[32];
  unsigned at = 0;  // the line the next byte belongs to
  unsigned n = 0;   // bytes of the wanted line copied
  while (at <= line) {
    const std::size_t got = inf.read(reinterpret_cast<std::uint8_t*>(buf), sizeof buf);
    if (got == 0u || inf.failed()) {
      out[0] = '\0';
      return 0u;
    }
    for (std::size_t i = 0; i < got && at <= line; ++i) {
      if (buf[i] == '\n') {
        ++at;
      } else if (at == line && n + 1u < cap) {
        out[n++] = buf[i];
      }
    }
  }
  out[n] = '\0';
  return n;
}

}  // namespace

#ifndef O3OS_FX_TEXT_CACHE_EXTERNAL
namespace {
FxTextCache g_fx_text_cache;  // host, the simulator and QEMU (the MK66 image: the platform's, app_main.cpp)
}  // namespace
FxTextCache& fx_text_cache() noexcept { return g_fx_text_cache; }
#endif

namespace {

// Decodes the names section (the first kPickerCount lines of the stream) into c.names, each line's newline as a NUL. False if the stream fails or the names do not fit.
bool decode_names(FxTextCache& c) noexcept {
  core::Inflater& inf = ui::asset_inflater();
  inf.begin(kFxTextZ, kFxTextZSize, kFxTextRawSize);
  char buf[32];
  unsigned lines = 0;
  unsigned n = 0;
  while (lines < model::fx::kPickerCount) {
    const std::size_t got = inf.read(reinterpret_cast<std::uint8_t*>(buf), sizeof buf);
    if (got == 0u || inf.failed()) {
      return false;
    }
    for (std::size_t i = 0; i < got && lines < model::fx::kPickerCount; ++i) {
      if (n >= kFxNamesBytes) {
        return false;
      }
      if (buf[i] == '\n') {
        c.names[n++] = '\0';
        ++lines;
      } else {
        c.names[n++] = buf[i];
      }
    }
  }
  return true;
}

// Copies the NUL-ended `src` into out/cap, cut at cap; returns the length.
unsigned copy_cut(const char* src, char* out, unsigned cap) noexcept {
  unsigned n = 0;
  while (src[n] != '\0' && n + 1u < cap) {
    out[n] = src[n];
    ++n;
  }
  out[n] = '\0';
  return n;
}

}  // namespace

unsigned fx_picker_name(unsigned index, char* out, unsigned cap) noexcept {
  if (cap == 0u) {
    return 0u;
  }
  if (index >= model::fx::kPickerCount) {
    index = 0u;
  }
  FxTextCache& c = fx_text_cache();
  if (c.names_state == 0u) {
    c.names_state = decode_names(c) ? 1u : 2u;
  }
  if (c.names_state != 1u) {
    return fx_line(index, out, cap);
  }
  const char* p = c.names;
  for (unsigned i = 0; i < index; ++i) {
    while (*p != '\0') {
      ++p;
    }
    ++p;
  }
  return copy_cut(p, out, cap);
}

unsigned fx_picker_desc(unsigned index, char* out, unsigned cap) noexcept {
  if (cap == 0u) {
    return 0u;
  }
  if (index >= model::fx::kPickerCount) {
    index = 0u;
  }
  FxTextCache& c = fx_text_cache();
  if (!c.desc_ok || c.desc_index != index) {
    c.desc_ok = 0u;
    if (fx_line(model::fx::kPickerCount + index, c.desc, kFxDescMax) == 0u) {
      out[0] = '\0';
      return 0u;
    }
    c.desc_index = static_cast<std::uint16_t>(index);
    c.desc_ok = 1u;
  }
  return copy_cut(c.desc, out, cap);
}

}  // namespace o3os::app
