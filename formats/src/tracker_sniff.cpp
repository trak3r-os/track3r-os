// SPDX-License-Identifier: GPL-3.0-or-later
// OS-FLASHDIET I3: the probe side of the MOD / S3M / MED importers, in the image's formats library. The importers themselves
// (formats/mod, formats/med) moved to the formats_tracker library, which the image does not link: it reads those files only through
// the importer module of the card (modules/importers/tracker.tmod). What the image still needs of their code is here, and only here:
// mod::sniff_kind (with its two helpers tagged_mod_min_bytes and is_soundtracker15, which ModuleImport::decide calls to offer the
// file on the IMPORT page) and the two drop_group_name tables (ModuleSummary::group_name names the report's dropped groups).
// The reader side of the same sources (formats_tracker) calls is_soundtracker15 and mod_tag from here as well, so both sides share this one copy.
#include <o3os/formats/med/import.h>
#include <o3os/formats/mod/import.h>

#include <algorithm>
#include <cstring>

namespace o3os::formats {

namespace mod {

namespace {

inline std::uint32_t be16(const std::uint8_t* p) noexcept { return (static_cast<std::uint32_t>(p[0]) << 8) | p[1]; }

// The 15-sample Soundtracker layout (OS-IMPORT-ANY I6): the title (20) and 15 sample headers (30 each) as in a 31-sample file, then the song length at 470, the restart byte, 128 orders at 472: 600 bytes.
constexpr std::uint32_t kSt15Samples = 15;
constexpr std::uint32_t kSt15HeaderBytes = 600;
constexpr std::uint32_t kSt15SongAt = 470;
constexpr std::uint32_t kSt15PatternBytes = caps::kRows * 4u * 4u;

bool printable_or_nul(const std::uint8_t* p, std::size_t n) noexcept {
  for (std::size_t i = 0; i < n; ++i) {
    if (p[i] != 0 && (p[i] < 0x20 || p[i] > 0x7E)) {
      return false;
    }
  }
  return true;
}

bool is_digit(std::uint8_t c) noexcept { return c >= '0' && c <= '9'; }

}  // namespace

// The tag at 1080 of a MOD: the variant and the channel count; false when it is none this reader takes. The one copy: the reader side
// (formats/mod/src/module.cpp, in formats_tracker) calls it from here, so the image's and the module's maps both carry this symbol.
bool mod_tag(const std::uint8_t* t, Variant& v, std::uint8_t& channels) noexcept {
  if (std::memcmp(t, "M.K.", 4) == 0 || std::memcmp(t, "M&K!", 4) == 0) {
    v = Variant::Mk;
    channels = 4;
  } else if (std::memcmp(t, "M!K!", 4) == 0) {
    v = Variant::Mk2;
    channels = 4;
  } else if (std::memcmp(t, "N.T.", 4) == 0) {
    v = Variant::Nt;
    channels = 4;
  } else if (std::memcmp(t, "FLT4", 4) == 0) {
    v = Variant::Flt4;
    channels = 4;
  } else if (std::memcmp(t, "FLT8", 4) == 0) {
    v = Variant::Flt8;
    channels = 8;
  } else if (t[0] >= '1' && t[0] <= '9' && std::memcmp(t + 1, "CHN", 3) == 0) {
    v = Variant::Chn;
    channels = static_cast<std::uint8_t>(t[0] - '0');
  } else if (is_digit(t[0]) && is_digit(t[1]) && std::memcmp(t + 2, "CH", 2) == 0) {
    const unsigned n = static_cast<unsigned>(t[0] - '0') * 10u + static_cast<unsigned>(t[1] - '0');
    if (n < 10 || n > caps::kMaxChannels) {
      return false;
    }
    v = Variant::Chn;
    channels = static_cast<std::uint8_t>(n);
  } else {
    return false;
  }
  return true;
}

Kind sniff_kind(const std::uint8_t* head, std::size_t n) noexcept {
  if (n >= 48 && std::memcmp(head + 44, "SCRM", 4) == 0 && head[28] == 0x1A && head[29] == 16) {
    return Kind::S3m;
  }
  Variant v;
  std::uint8_t channels;
  if (n >= caps::kModHeaderBytes && mod_tag(head + 1080, v, channels)) {
    return Kind::Mod;
  }
  return Kind::None;
}

std::uint32_t tagged_mod_min_bytes(const std::uint8_t* head, std::size_t n) noexcept {
  Variant v;
  std::uint8_t channels;
  if (n < caps::kModHeaderBytes || !mod_tag(head + 1080, v, channels)) {
    return 0;
  }
  return caps::kModHeaderBytes + caps::kRows * 4u * channels;  // the header and one pattern (64 rows of 4 bytes a channel)
}

bool is_soundtracker15(const std::uint8_t* head, std::size_t n, std::uint32_t file_size) noexcept {
  if (n < kSt15HeaderBytes || file_size < kSt15HeaderBytes || !printable_or_nul(head, 20)) {
    return false;
  }
  std::uint64_t data = 0;
  std::uint32_t last = 0;  // the data of the last sample that has any
  for (std::uint32_t i = 0; i < kSt15Samples; ++i) {
    const std::uint8_t* rec = head + 20 + 30 * i;
    if (!printable_or_nul(rec, 22) || rec[25] > 64) {
      return false;
    }
    const std::uint32_t len_words = be16(rec + 22);
    // a loop that starts beyond the sample is nonsense whatever the unit of its start (words, or bytes in the oldest files): not a sample header
    if (len_words != 0 && be16(rec + 28) > 1 && be16(rec + 26) > 2u * len_words) {
      return false;
    }
    const std::uint32_t len = 2u * len_words;
    data += len;
    last = len != 0 ? len : last;
  }
  const std::uint32_t song_len = head[kSt15SongAt];
  if (song_len == 0 || song_len > caps::kMaxModOrders) {
    return false;
  }
  // 22 bytes of printable text with no NUL at the song length is the name of the 16th sample of a 31-sample file with a tag this reader does not know
  // (a song length of 32..126, a restart byte and 20 orders that are all patterns 32 and up is not a song)
  bool name_like = true;
  for (std::uint32_t i = 0; i < 22; ++i) {
    name_like = name_like && head[kSt15SongAt + i] >= 0x20 && head[kSt15SongAt + i] <= 0x7E;
  }
  if (name_like) {
    return false;
  }
  std::uint32_t max_all = 0, max_used = 0;
  for (std::uint32_t i = 0; i < caps::kMaxModOrders; ++i) {
    const std::uint32_t v = head[kSt15SongAt + 2 + i];
    max_all = std::max(max_all, v);
    if (i < song_len) {
      if (v >= 64) {
        return false;
      }
      max_used = std::max(max_used, v);
    }
  }
  std::uint32_t count = max_all < 64 ? max_all + 1 : max_used + 1;
  if (static_cast<std::uint64_t>(kSt15HeaderBytes) + static_cast<std::uint64_t>(count) * kSt15PatternBytes > file_size) {
    count = max_used + 1;  // a few tools leave junk behind the song length (as for a 31-sample file)
  }
  const std::uint64_t fixed = static_cast<std::uint64_t>(kSt15HeaderBytes) + static_cast<std::uint64_t>(count) * kSt15PatternBytes;
  return fixed <= file_size && data - last <= file_size - fixed;  // every sample whole but the last
}

const char* drop_group_name(unsigned index) noexcept {
  static const char* const kNames[] = {
      "AdLib instruments", "packed samples", "stereo samples", "instruments refused", "channels without a track", "effects without a counterpart", "effects approximated",
      "global or master volume", "slide timing",   "loop point",     "flow",                "periods off the table",    "restart position",
  };
  static_assert(sizeof kNames / sizeof kNames[0] == kDropGroupCount, "a name for every group");
  return index < kDropGroupCount ? kNames[index] : "";
}

}  // namespace mod

namespace med {

const char* drop_group_name(unsigned index) noexcept {
  static const char* const kNames[kDropGroupCount] = {
      "annotation",        "notation",           "MIDI dumps",          "MMDInfo text",     "ARexx",
      "MIDI 3x settings",  "echo",               "volume adjust",       "stereo separation", "extra songs",
      "channel split",     "master volume",      "tracks without a slot", "commands",       "synth tables",
      "hold and decay",    "default pitch",      "MIDI program",        "loop point",       "instruments refused",
      "flow",
  };
  return index < kDropGroupCount ? kNames[index] : "";
}

}  // namespace med

}  // namespace o3os::formats
