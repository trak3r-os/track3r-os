// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/formats/mod/module.h>

#include <algorithm>
#include <cstring>

namespace o3os::formats::mod {

const std::uint16_t kPeriodTable[36] = {856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453,
                                        428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226,
                                        214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113};

namespace {

inline std::uint32_t be16(const std::uint8_t* p) noexcept { return (static_cast<std::uint32_t>(p[0]) << 8) | p[1]; }
inline std::uint32_t le16(const std::uint8_t* p) noexcept { return (static_cast<std::uint32_t>(p[1]) << 8) | p[0]; }
inline std::uint32_t le32(const std::uint8_t* p) noexcept {
  return (static_cast<std::uint32_t>(p[3]) << 24) | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[1]) << 8) | p[0];
}
inline Status fail(Error e, std::uint32_t offset, const char* detail) noexcept { return Status{e, offset, detail}; }

// The 15-sample Soundtracker layout (OS-IMPORT-ANY I6): the title (20) and 15 sample headers (30 each) as in a 31-sample file, then the song length at 470, the restart byte, 128 orders at 472: 600 bytes.
constexpr std::uint32_t kSt15Samples = 15;
constexpr std::uint32_t kSt15HeaderBytes = 600;
constexpr std::uint32_t kSt15SongAt = 470;
constexpr std::uint32_t kMkSongAt = 950;  // the same fields of a 31-sample file
constexpr std::uint32_t kSt15PatternBytes = caps::kRows * 4u * 4u;

bool printable_or_nul(const std::uint8_t* p, std::size_t n) noexcept {
  for (std::size_t i = 0; i < n; ++i) {
    if (p[i] != 0 && (p[i] < 0x20 || p[i] > 0x7E)) {
      return false;
    }
  }
  return true;
}

// A fixed-width name field: up to the first NUL, trailing spaces cut, bytes outside 0x20..0x7E as '?'. `out` has room for cap + 1.
void copy_name(const std::uint8_t* field, std::size_t field_len, char* out, std::size_t cap) noexcept {
  std::size_t n = 0;
  while (n < field_len && field[n] != 0) {
    ++n;
  }
  while (n > 0 && field[n - 1] == ' ') {
    --n;
  }
  n = std::min(n, cap);
  for (std::size_t i = 0; i < n; ++i) {
    out[i] = (field[i] >= 0x20 && field[i] <= 0x7E) ? static_cast<char>(field[i]) : '?';
  }
  out[n] = '\0';
}

bool is_digit(std::uint8_t c) noexcept { return c >= '0' && c <= '9'; }

// The tag at 1080 of a MOD: the variant and the channel count; false when it is none this reader takes.
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

}  // namespace

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

// ---- plumbing ----------------------------------------------------------------------------------------------------------

Status Module::get(std::uint32_t offset, void* dst, std::uint32_t n) const noexcept {
  if (static_cast<std::uint64_t>(offset) + n > info_.file_size) {
    return fail(Error::Truncated, offset, "read past the end of the file");
  }
  if (n != 0 && !src_->read_at(offset, dst, n)) {
    return fail(Error::SourceFailed, offset, "the byte source failed");
  }
  return Status{};
}

Status Module::open(RandomSource& source) noexcept {
  src_ = &source;
  info_ = ModuleInfo();
  std::memset(order_, 0, sizeof order_);
  std::memset(ins_ptr_, 0, sizeof ins_ptr_);
  std::memset(pat_ptr_, 0, sizeof pat_ptr_);
  cached_ = 0xFFFFFFFFu;
  decoded_ = 0;
  pat_offset_ = 0;
  sample_offset_ = 0;
  std::memset(smp_off_, 0, sizeof smp_off_);
  info_.file_size = source.size();
  Status s;
  std::uint8_t head[caps::kModHeaderBytes];
  const std::uint32_t n = std::min<std::uint32_t>(info_.file_size, caps::kModHeaderBytes);
  s = get(0, head, n);
  if (!s.ok()) {
    return s;
  }
  if (n >= caps::kS3mHeaderBytes && std::memcmp(head + 44, "SCRM", 4) == 0) {
    s = open_s3m(head);
  } else if (n >= caps::kModHeaderBytes) {
    s = open_mod(head);
  } else {
    s = fail(Error::BadMagic, 0, "not a module: too short for a MOD or S3M header");
  }
  if (!s.ok()) {
    info_ = ModuleInfo();
  }
  return s;
}

// ---- MOD ---------------------------------------------------------------------------------------------------------------

Status Module::open_mod(const std::uint8_t* h) noexcept {
  Variant v = Variant::None;
  std::uint8_t channels = 0;
  // A file with no tag this reader knows is a 15-sample Soundtracker module when its structure says so (OS-IMPORT-ANY I6): the same fields, 600 bytes of header, 15 samples, 4 channels.
  const bool st15 = !mod_tag(h + 1080, v, channels);
  if (st15) {
    if (!is_soundtracker15(h, info_.file_size < caps::kModHeaderBytes ? info_.file_size : caps::kModHeaderBytes, info_.file_size)) {
      return fail(Error::BadMagic, 1080, "not a module this reader knows: no M.K., M!K!, N.T., FLT4/8, xCHN or xxCH signature, and not the structure of a 15-sample Soundtracker file");
    }
    v = Variant::St15;
    channels = 4;
  }
  const std::uint32_t head_bytes = st15 ? kSt15HeaderBytes : caps::kModHeaderBytes;  // the header, where the patterns begin
  const std::uint32_t song_at = st15 ? kSt15SongAt : kMkSongAt;                      // the song length; the restart byte follows it and then the 128 orders
  const std::uint32_t samples = st15 ? kSt15Samples : caps::kModSamples;
  const std::uint32_t song_len = h[song_at];
  if (song_len == 0 || song_len > caps::kMaxModOrders) {
    return fail(Error::Malformed, 950, "the song length is 0 or above 128");
  }
  const bool flt8 = v == Variant::Flt8;
  // The patterns the file holds: one more than the highest number of the whole order table (the file's layout counts all 128 entries); a few tools leave junk
  // behind the song length, so when that count does not fit the file, the entries the song plays are all that count.
  std::uint32_t max_all = 0, max_used = 0;
  for (std::uint32_t i = 0; i < caps::kMaxModOrders; ++i) {
    max_all = std::max<std::uint32_t>(max_all, h[song_at + 2 + i]);
    if (i < song_len) {
      max_used = std::max<std::uint32_t>(max_used, h[song_at + 2 + i]);
    }
  }
  if (max_all > caps::kMaxModPatternNumber && max_used > caps::kMaxModPatternNumber) {
    return fail(Error::LimitExceeded, 952, "a pattern number above 127");
  }
  const std::uint32_t pat_bytes = caps::kRows * channels * 4u;
  auto count_of = [flt8](std::uint32_t max) { return flt8 ? (max >> 1) + 1u : max + 1u; };
  // (a 15-sample file counts the whole table only when none of it is junk, 64 and up: the same rule as is_soundtracker15's)
  const bool whole_table = st15 ? max_all < 64 : max_all <= caps::kMaxModPatternNumber;
  std::uint32_t count = whole_table ? count_of(max_all) : count_of(max_used);
  if (static_cast<std::uint64_t>(head_bytes) + static_cast<std::uint64_t>(count) * pat_bytes > info_.file_size) {
    count = count_of(max_used);
  }
  if (static_cast<std::uint64_t>(head_bytes) + static_cast<std::uint64_t>(count) * pat_bytes > info_.file_size) {
    return fail(Error::Truncated, head_bytes, "the file ends inside the pattern data");
  }
  info_.kind = Kind::Mod;
  info_.variant = v;
  if (st15) {
    std::memcpy(info_.tag, "ST15", 5);  // no signature in the file: this says what it was read as
  } else {
    std::memcpy(info_.tag, h + 1080, 4);
    info_.tag[4] = '\0';
  }
  for (int i = 0; i < 4; ++i) {
    if (info_.tag[i] < 0x20 || info_.tag[i] > 0x7E) {
      info_.tag[i] = '?';
    }
  }
  info_.channels = channels;
  info_.order_count = static_cast<std::uint16_t>(song_len);
  info_.pattern_count = static_cast<std::uint16_t>(count);
  info_.sample_count = static_cast<std::uint16_t>(samples);
  info_.restart = st15 ? std::uint8_t{127} : h[song_at + 1];  // (a Soundtracker file keeps another value there: none)
  copy_name(h, 20, info_.name, 20);
  for (std::uint32_t i = 0; i < song_len; ++i) {
    order_[i] = static_cast<std::uint8_t>(flt8 ? h[song_at + 2 + i] >> 1 : h[song_at + 2 + i]);
  }
  pat_offset_ = head_bytes;
  sample_offset_ = head_bytes + count * pat_bytes;
  std::uint32_t at = sample_offset_;  // the data of the samples follow one another, in the order of the headers
  for (std::uint32_t i = 0; i < samples; ++i) {
    smp_off_[i] = at;
    at += 2u * be16(h + 20 + 30 * i + 22);  // at most 31 x 131,070 bytes on top of a 16 MiB bound: no overflow
  }
  return Status{};
}

// ---- S3M ---------------------------------------------------------------------------------------------------------------

Status Module::open_s3m(const std::uint8_t* h) noexcept {
  if (h[28] != 0x1A || h[29] != 16) {
    return fail(Error::BadMagic, 28, "an SCRM signature, but not the header of a Scream Tracker 3 module (0x1A, type 16)");
  }
  const std::uint32_t ord_num = le16(h + 32), ins_num = le16(h + 34), pat_num = le16(h + 36);
  if (ord_num > caps::kMaxS3mOrders || ins_num > caps::kMaxS3mInstruments || pat_num > caps::kMaxS3mPatterns) {
    return fail(Error::LimitExceeded, 32, "more orders, instruments or patterns than a Scream Tracker 3 module holds (255)");
  }
  const std::uint8_t dp = h[53];
  const std::uint64_t tables = static_cast<std::uint64_t>(caps::kS3mHeaderBytes) + ord_num + 2ull * ins_num + 2ull * pat_num + (dp == 252 ? 32u : 0u);
  if (tables > info_.file_size) {
    return fail(Error::Truncated, caps::kS3mHeaderBytes, "the file ends inside the order list or the parapointer tables");
  }
  info_.kind = Kind::S3m;
  info_.variant = Variant::S3m;
  std::memcpy(info_.tag, "SCRM", 5);
  copy_name(h, 28, info_.name, 28);
  info_.cwtv = static_cast<std::uint16_t>(le16(h + 40));
  info_.signed_samples = le16(h + 42) == 1;
  const std::uint32_t flags = le16(h + 38);
  info_.fast_slides = (flags & 0x40u) != 0 || (info_.cwtv == 0x1300);
  info_.amiga_limits = (flags & 0x10u) != 0;
  info_.global_volume = h[48];
  info_.speed = h[49] != 0 ? h[49] : std::uint8_t{6};
  info_.tempo = h[50] >= 32 ? h[50] : std::uint8_t{32};  // below 32 ST3 plays 32 (libopenmpt measured: tempo 0 and 20 both take as long as 32)
  info_.master_volume = h[51];
  info_.stereo = (h[51] & 0x80u) != 0;
  info_.sample_count = static_cast<std::uint16_t>(ins_num);
  info_.pattern_count = static_cast<std::uint16_t>(pat_num);

  // channels
  std::uint32_t top = 0;
  for (std::uint32_t c = 0; c < caps::kMaxChannels; ++c) {
    const std::uint8_t set = h[64 + c];
    if (set == 255) {
      continue;
    }
    info_.chan_used[c] = true;
    top = c + 1;
    const std::uint32_t type = set & 0x7Fu;
    info_.chan_muted[c] = (set & 0x80u) != 0;
    info_.chan_adlib[c] = type >= 16;
    info_.muted_channels = static_cast<std::uint8_t>(info_.muted_channels + (info_.chan_muted[c] ? 1u : 0u));
    info_.adlib_channels = static_cast<std::uint8_t>(info_.adlib_channels + (info_.chan_adlib[c] ? 1u : 0u));
    // default panning: left channels 3 of 15, right channels 12 (ST3's own), centre for a mono module
    std::uint32_t pct = 50;
    if (info_.stereo && type < 16) {
      pct = ((type < 8 ? 3u : 12u) * 100u + 7u) / 15u;
    }
    info_.chan_pan[c] = static_cast<std::uint8_t>(pct);
  }
  if (dp == 252) {
    std::uint8_t pan[32];
    const std::uint32_t at = static_cast<std::uint32_t>(tables) - 32u;
    Status s = get(at, pan, 32);
    if (!s.ok()) {
      return s;
    }
    for (std::uint32_t c = 0; c < caps::kMaxChannels; ++c) {
      if ((pan[c] & 0x20u) != 0 && info_.chan_used[c]) {
        info_.chan_pan[c] = static_cast<std::uint8_t>(((pan[c] & 0x0Fu) * 100u + 7u) / 15u);
      }
    }
  }
  info_.channels = static_cast<std::uint8_t>(top);

  // order list, instrument and pattern parapointers
  std::uint8_t buf[256];
  Status s = get(caps::kS3mHeaderBytes, buf, ord_num);
  if (!s.ok()) {
    return s;
  }
  std::uint32_t count = 0;
  for (std::uint32_t i = 0; i < ord_num; ++i) {
    const std::uint8_t o = buf[i];
    if (o == 255) {
      break;  // the end of the song: what follows is not played
    }
    if (o == 254) {
      info_.skipped_orders = static_cast<std::uint8_t>(std::min<unsigned>(255u, info_.skipped_orders + 1u));
      continue;
    }
    if (o >= pat_num) {
      info_.bad_orders = static_cast<std::uint8_t>(std::min<unsigned>(255u, info_.bad_orders + 1u));
      continue;
    }
    order_[count++] = o;
  }
  info_.order_count = static_cast<std::uint16_t>(count);
  std::uint32_t at = caps::kS3mHeaderBytes + ord_num;
  for (std::uint32_t i = 0; i < ins_num; ++i) {
    s = get(at + 2u * i, buf, 2);
    if (!s.ok()) {
      return s;
    }
    ins_ptr_[i] = static_cast<std::uint16_t>(le16(buf));
  }
  at += 2u * ins_num;
  for (std::uint32_t i = 0; i < pat_num; ++i) {
    s = get(at + 2u * i, buf, 2);
    if (!s.ok()) {
      return s;
    }
    pat_ptr_[i] = static_cast<std::uint16_t>(le16(buf));
  }
  return Status{};
}

// ---- rows --------------------------------------------------------------------------------------------------------------

Status Module::mod_line(std::uint32_t pattern, std::uint32_t row, Cell* out) const noexcept {
  const std::uint32_t ch = info_.channels;
  std::uint8_t buf[caps::kMaxChannels * 4];
  const std::uint32_t pat_bytes = caps::kRows * ch * 4u;
  const std::uint32_t base = pat_offset_ + pattern * pat_bytes;
  Status s;
  if (info_.variant == Variant::Flt8) {  // two blocks of 4 channels: channels 0-3 in the first, 4-7 in the second
    s = get(base + row * 16u, buf, 16);
    if (s.ok()) {
      s = get(base + 1024u + row * 16u, buf + 16, 16);
    }
  } else {
    s = get(base + row * ch * 4u, buf, ch * 4u);
  }
  if (!s.ok()) {
    return s;
  }
  for (std::uint32_t c = 0; c < ch; ++c) {
    const std::uint8_t* b = buf + 4u * c;
    Cell& cell = out[c];
    cell = Cell();
    cell.period = static_cast<std::uint16_t>(((b[0] & 0x0Fu) << 8) | b[1]);
    cell.instrument = static_cast<std::uint8_t>((b[0] & 0xF0u) | (b[2] >> 4));
    cell.effect = b[2] & 0x0Fu;
    cell.param = b[3];
    if (cell.period != 0) {
      std::uint32_t best = 0, best_d = 0xFFFFFFFFu;
      for (std::uint32_t i = 0; i < 36; ++i) {
        const std::uint32_t d = cell.period > kPeriodTable[i] ? cell.period - kPeriodTable[i] : kPeriodTable[i] - cell.period;
        if (d < best_d) {
          best_d = d;
          best = i;
        }
      }
      cell.note = static_cast<std::uint8_t>(best + 1u);
      cell.off_table = best_d != 0;
    }
  }
  return Status{};
}

// The decoder of one packed S3M pattern into cache_ (docs/formats/mod.md): [u16 length including itself] then rows of entries, each row ended by a 0 byte; an entry byte is
// channel (bits 0-4), 0x20 note + instrument follow, 0x40 volume follows, 0x80 effect + parameter follow.
Status Module::decode_s3m(std::uint32_t pattern) noexcept {
  for (std::uint32_t i = 0; i < caps::kRows * caps::kMaxChannels; ++i) {
    std::uint8_t* c = cache_ + 5u * i;
    c[0] = 255;  // note: none
    c[1] = 0;
    c[2] = 255;  // volume: none
    c[3] = 0;
    c[4] = 0;
  }
  cached_ = 0xFFFFFFFFu;
  const std::uint32_t para = pat_ptr_[pattern];
  if (para != 0) {
    const std::uint32_t offset = para * 16u;
    std::uint8_t head[2];
    Status s = get(offset, head, 2);
    if (!s.ok()) {
      return s;
    }
    const std::uint32_t len = le16(head);
    if (len < 2) {
      return fail(Error::Malformed, offset, "a packed pattern shorter than its own length field");
    }
    if (static_cast<std::uint64_t>(offset) + len > info_.file_size) {
      return fail(Error::Truncated, offset, "the file ends inside a packed pattern");
    }
    decoded_ += len;
    if (decoded_ > caps::kDecodeBudget) {
      return fail(Error::LimitExceeded, offset, "the patterns made more decoding work than the cap");
    }
    std::uint32_t left = len - 2u;
    std::uint32_t at = offset + 2u;
    std::uint8_t win[256];
    std::uint32_t pos = 0, have = 0;
    auto next = [&](std::uint8_t& v) -> bool {
      if (pos == have) {
        if (left == 0) {
          return false;
        }
        const std::uint32_t n = std::min<std::uint32_t>(left, sizeof win);
        if (!get(at, win, n).ok()) {
          left = 0;
          return false;
        }
        at += n;
        left -= n;
        pos = 0;
        have = n;
      }
      v = win[pos++];
      return true;
    };
    std::uint32_t row = 0;
    while (row < caps::kRows) {
      std::uint8_t b;
      if (!next(b)) {
        break;  // the data ended early: the rows that are left are empty
      }
      if (b == 0) {
        ++row;
        continue;
      }
      std::uint8_t* c = cache_ + 5u * (row * caps::kMaxChannels + (b & 31u));
      std::uint8_t v;
      if ((b & 0x20u) != 0) {
        if (!next(v)) break;
        c[0] = v;
        if (!next(v)) break;
        c[1] = v;
      }
      if ((b & 0x40u) != 0) {
        if (!next(v)) break;
        c[2] = v;
      }
      if ((b & 0x80u) != 0) {
        if (!next(v)) break;
        c[3] = v;
        if (!next(v)) break;
        c[4] = v;
      }
    }
  }
  cached_ = pattern;
  return Status{};
}

Status Module::read_line(std::uint32_t pattern, std::uint32_t row, Cell* out) noexcept {
  if (info_.kind == Kind::None || pattern >= info_.pattern_count || row >= caps::kRows || out == nullptr) {
    return fail(Error::Malformed, 0, "a row outside the module");
  }
  if (info_.kind == Kind::Mod) {
    return mod_line(pattern, row, out);
  }
  if (cached_ != pattern) {
    const Status s = decode_s3m(pattern);
    if (!s.ok()) {
      return s;
    }
  }
  for (std::uint32_t c = 0; c < info_.channels; ++c) {
    const std::uint8_t* b = cache_ + 5u * (row * caps::kMaxChannels + c);
    Cell& cell = out[c];
    cell = Cell();
    if (b[0] == 254) {
      cell.cut = true;
    } else if (b[0] != 255) {
      // octave x 12 + semitone + 1, as libopenmpt reads it: a semitone above 11 (ST3 never writes one) is the next octave's
      cell.note = static_cast<std::uint8_t>((b[0] >> 4) * 12u + (b[0] & 15u) + 1u);
      cell.bad_note = (b[0] & 15u) > 11u;
    }
    cell.instrument = b[1];
    cell.volume = b[2];
    cell.effect = b[3];
    cell.param = b[4];
  }
  return Status{};
}

// ---- samples -----------------------------------------------------------------------------------------------------------

Status Module::sample(std::uint32_t slot, SampleInfo& out) const noexcept {
  out = SampleInfo();
  if (info_.kind == Kind::None || slot >= info_.sample_count) {
    return fail(Error::Malformed, 0, "a sample outside the module");
  }
  if (info_.kind == Kind::Mod) {
    std::uint8_t h[30];
    Status s = get(20u + 30u * slot, h, 30);
    if (!s.ok()) {
      return s;
    }
    copy_name(h, 22, out.name, 22);
    const std::uint64_t offset = smp_off_[slot];
    const std::uint32_t len = 2u * be16(h + 22);
    out.volume = static_cast<std::uint8_t>(std::min<unsigned>(h[25], 64u));
    const unsigned nib = h[24] & 0x0Fu;
    out.finetune = static_cast<std::int8_t>(nib >= 8 ? static_cast<int>(nib) - 16 : static_cast<int>(nib));
    std::uint32_t frames = len;
    const std::uint64_t avail = offset < info_.file_size ? info_.file_size - offset : 0;
    if (frames > avail) {
      frames = static_cast<std::uint32_t>(avail);
      out.notes |= kNoteTrimmed;
    }
    if (frames == 0) {
      return Status{};  // Empty
    }
    out.kind = SampleKind::Pcm;
    out.offset = static_cast<std::uint32_t>(offset);
    out.frames = frames;
    out.is_signed = true;
    const std::uint32_t rep_len = 2u * be16(h + 28);
    if (be16(h + 28) > 1) {
      const std::uint32_t start = 2u * be16(h + 26);
      std::uint32_t end = start + rep_len;  // < 2^19: no overflow
      if (end > frames) {
        end = frames;
        out.notes |= kNoteLoopClamped;
      }
      if (start < end && end - start >= 2) {
        out.has_loop = true;
        out.loop_start = start;
        out.loop_len = end - start;
      } else {
        out.notes |= kNoteLoopClamped;
      }
    }
    return Status{};
  }
  // S3M
  const std::uint32_t para = ins_ptr_[slot];
  if (para == 0) {
    return Status{};
  }
  const std::uint32_t at = para * 16u;
  std::uint8_t h[80];
  Status s = get(at, h, 80);
  if (!s.ok()) {
    return s;
  }
  out.s3m_type = h[0];
  copy_name(h + 48, 28, out.name, 28);
  if (h[0] == 0) {
    return Status{};
  }
  if (h[0] >= 2) {
    out.kind = SampleKind::AdLib;
    return Status{};
  }
  out.volume = static_cast<std::uint8_t>(std::min<unsigned>(h[28], 64u));
  out.c2spd = le32(h + 32);
  if (h[30] != 0) {  // 1 = ADPCM: nothing here decodes it
    out.kind = SampleKind::Unsupported;
    return Status{};
  }
  const std::uint32_t flags = h[31];
  out.sixteen_bit = (flags & 4u) != 0;
  out.is_signed = info_.signed_samples;
  if ((flags & 2u) != 0) {
    out.notes |= kNoteStereo;
  }
  const std::uint64_t offset = 16ull * ((static_cast<std::uint64_t>(h[13]) << 16) | le16(h + 14));
  std::uint32_t frames = le32(h + 16);
  const std::uint32_t bpf = out.sixteen_bit ? 2u : 1u;
  const std::uint64_t avail = offset < info_.file_size ? info_.file_size - offset : 0;
  if (static_cast<std::uint64_t>(frames) * bpf > avail) {
    frames = static_cast<std::uint32_t>(avail / bpf);
    out.notes |= kNoteTrimmed;
  }
  if (frames == 0) {
    return Status{};
  }
  out.kind = SampleKind::Pcm;
  out.offset = static_cast<std::uint32_t>(offset);
  out.frames = frames;
  if ((flags & 1u) != 0) {
    const std::uint32_t start = le32(h + 20);
    std::uint32_t end = le32(h + 24);
    if (end > frames) {
      end = frames;
      out.notes |= kNoteLoopClamped;
    }
    if (start < end && end - start >= 2) {
      out.has_loop = true;
      out.loop_start = start;
      out.loop_len = end - start;
    } else {
      out.notes |= kNoteLoopClamped;
    }
  }
  return Status{};
}

Status Module::read_pcm(const SampleInfo& s, std::uint32_t first, std::uint32_t n, std::int16_t* dst) const noexcept {
  if (s.kind != SampleKind::Pcm || first > s.frames || n > s.frames - first || (n != 0 && dst == nullptr)) {
    return fail(Error::Malformed, s.offset, "PCM range outside the sample");
  }
  const std::uint32_t bpf = s.sixteen_bit ? 2u : 1u;
  std::uint8_t buf[512];
  const std::uint32_t chunk = sizeof buf / bpf;
  std::uint32_t done = 0;
  while (done < n) {
    const std::uint32_t k = std::min(chunk, n - done);
    const Status st = get(s.offset + (first + done) * bpf, buf, k * bpf);
    if (!st.ok()) {
      return st;
    }
    for (std::uint32_t i = 0; i < k; ++i) {
      int v;
      if (s.sixteen_bit) {
        v = static_cast<int>(le16(buf + 2 * i));
        v = s.is_signed ? (v >= 32768 ? v - 65536 : v) : v - 32768;
      } else {
        v = s.is_signed ? static_cast<int>(static_cast<std::int8_t>(buf[i])) : static_cast<int>(buf[i]) - 128;
        v *= 256;
      }
      dst[done + i] = static_cast<std::int16_t>(v);
    }
    done += k;
  }
  return Status{};
}

}  // namespace o3os::formats::mod
