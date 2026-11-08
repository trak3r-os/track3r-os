// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/core/inflate.h>

namespace o3os::core {

namespace {

// The order the code-length code lengths of a dynamic block header arrive in (RFC 1951 3.2.7).
constexpr std::uint8_t kClOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

// Match length symbols 257..285 and distance symbols 0..29: base value and extra bits (RFC 1951 3.2.5).
constexpr std::uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                         257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                         8193, 12289, 16385, 24577};
constexpr std::uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                         7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

}  // namespace

void Inflater::begin(const std::uint8_t* z, std::size_t z_size, std::size_t raw_size) noexcept {
  z_ = z;
  z_size_ = z_size;
  raw_size_ = raw_size;
  z_next_ = 0;
  bitbuf_ = 0;
  bitcnt_ = 0;
  out_total_ = 0;
  wpos_ = 0;
  block_open_ = false;
  final_ = false;
  stored_mode_ = false;
  stored_left_ = 0;
  match_len_ = 0;
  match_dist_ = 0;
  state_ = State::kArmed;
}

std::size_t Inflater::read(std::uint8_t* dst, std::size_t n) noexcept {
  if (state_ != State::kArmed) {
    return 0;
  }
  return pump(dst, n);
}

std::size_t Inflater::skip(std::size_t n) noexcept {
  if (state_ != State::kArmed) {
    return 0;
  }
  return pump(nullptr, n);
}

int Inflater::bits(int need) noexcept {
  while (bitcnt_ < need) {
    if (z_next_ >= z_size_) {
      return -1;  // truncated
    }
    bitbuf_ |= static_cast<std::uint32_t>(z_[z_next_]) << bitcnt_;
    ++z_next_;
    bitcnt_ += 8;
  }
  const std::uint32_t mask = (static_cast<std::uint32_t>(1) << need) - 1;
  const int value = static_cast<int>(bitbuf_ & mask);
  bitbuf_ >>= need;
  bitcnt_ -= need;
  return value;
}

// Canonical-Huffman table construction by counting the codes per length and laying the symbols out
// sorted by (length, value) -- the technique zlib's puff.c also uses (an algorithm; no code is copied
// from it).
template <std::size_t N>
bool Inflater::huff_build(Huff<N>& h, const std::uint8_t* lengths, int n) noexcept {
  for (int len = 0; len < 16; ++len) {
    h.per_len[len] = 0;
  }
  for (int i = 0; i < n; ++i) {
    if (lengths[i] > 15) {
      return false;
    }
    ++h.per_len[lengths[i]];
  }
  h.per_len[0] = 0;  // length 0 means "no code", it is not a code length
  int avail = 1;  // code space left, doubled at each length
  for (int len = 1; len <= 15; ++len) {
    avail <<= 1;
    avail -= h.per_len[len];
    if (avail < 0) {
      return false;  // over-subscribed: more codes of these lengths than the code space has
    }
  }
  int base[16] = {};  // where each length's run of symbols starts in h.ranked
  for (int len = 1; len < 15; ++len) {
    base[len + 1] = base[len] + h.per_len[len];
  }
  for (int i = 0; i < n; ++i) {
    if (lengths[i] != 0) {
      h.ranked[base[lengths[i]]++] = static_cast<std::uint16_t>(i);
    }
  }
  return true;
}

template <std::size_t N>
int Inflater::huff_decode(const Huff<N>& h) noexcept {
  int acc = 0;   // the code read so far
  int lo = 0;    // the first code of this length
  int slot = 0;  // where this length's symbols start in h.ranked
  for (int len = 1; len <= 15; ++len) {
    const int bit = bits(1);
    if (bit < 0) {
      return -1;
    }
    acc |= bit;
    const int n = h.per_len[len];
    if (acc - lo < n) {
      return h.ranked[slot + (acc - lo)];
    }
    slot += n;
    lo = (lo + n) << 1;
    acc <<= 1;
  }
  return -2;  // no code of any length matches: an incomplete table met an unassigned code
}

void Inflater::build_fixed() noexcept {
  std::uint8_t lengths[288];
  int i = 0;
  for (; i < 144; ++i) {
    lengths[i] = 8;
  }
  for (; i < 256; ++i) {
    lengths[i] = 9;
  }
  for (; i < 280; ++i) {
    lengths[i] = 7;
  }
  for (; i < 288; ++i) {
    lengths[i] = 8;
  }
  huff_build(lit_, lengths, 288);
  std::uint8_t dlengths[32];
  for (i = 0; i < 32; ++i) {
    dlengths[i] = 5;
  }
  huff_build(dist_, dlengths, 32);
}

bool Inflater::parse_dynamic() noexcept {
  const int hlit = bits(5);
  const int hdist = bits(5);
  const int hclen = bits(4);
  if (hlit < 0 || hdist < 0 || hclen < 0) {
    return false;
  }
  const int nlit = hlit + 257;
  const int ndist = hdist + 1;
  const int ncl = hclen + 4;
  std::uint8_t cl_len[19] = {};
  for (int i = 0; i < ncl; ++i) {
    const int v = bits(3);
    if (v < 0) {
      return false;
    }
    cl_len[kClOrder[i]] = static_cast<std::uint8_t>(v);
  }
  if (!huff_build(cl_, cl_len, 19)) {
    return false;
  }
  std::uint8_t lengths[288 + 32];
  const int total = nlit + ndist;
  int i = 0;
  while (i < total) {
    const int sym = huff_decode(cl_);
    if (sym < 0) {
      return false;
    }
    if (sym < 16) {
      lengths[i++] = static_cast<std::uint8_t>(sym);
      continue;
    }
    int rep;
    std::uint8_t val;
    if (sym == 16) {
      if (i == 0) {
        return false;  // repeats with nothing to repeat
      }
      const int e = bits(2);
      if (e < 0) {
        return false;
      }
      rep = 3 + e;
      val = lengths[i - 1];
    } else if (sym == 17) {
      const int e = bits(3);
      if (e < 0) {
        return false;
      }
      rep = 3 + e;
      val = 0;
    } else {
      const int e = bits(7);
      if (e < 0) {
        return false;
      }
      rep = 11 + e;
      val = 0;
    }
    if (i + rep > total) {
      return false;
    }
    while (rep-- > 0) {
      lengths[i++] = val;
    }
  }
  if (lengths[256] == 0) {
    return false;  // symbol 256 (end of block) has no code: the block could never end
  }
  if (!huff_build(lit_, lengths, nlit)) {
    return false;
  }
  return huff_build(dist_, lengths + nlit, ndist);
}

bool Inflater::start_block() noexcept {
  const int bfinal = bits(1);
  const int btype = bits(2);
  if (bfinal < 0 || btype < 0) {
    state_ = State::kFailed;  // truncated inside a block header
    return false;
  }
  final_ = bfinal != 0;
  if (btype == 0) {
    const int drop = bitcnt_ & 7;  // a stored block skips to a byte boundary
    bitbuf_ >>= drop;
    bitcnt_ -= drop;
    const int len = bits(16);
    const int nlen = bits(16);
    if (len < 0 || nlen < 0 || (len ^ 0xFFFF) != nlen) {
      state_ = State::kFailed;
      return false;
    }
    stored_mode_ = true;
    stored_left_ = static_cast<std::uint16_t>(len);
    block_open_ = true;
    return true;
  }
  stored_mode_ = false;
  stored_left_ = 0;
  if (btype == 1) {
    build_fixed();
    block_open_ = true;
    return true;
  }
  if (btype == 2) {
    if (!parse_dynamic()) {
      state_ = State::kFailed;
      return false;
    }
    block_open_ = true;
    return true;
  }
  state_ = State::kFailed;  // BTYPE 3 is reserved
  return false;
}

void Inflater::finish() noexcept {
  if (out_total_ == raw_size_ && z_next_ == z_size_) {
    state_ = State::kDone;  // the final block ended exactly at raw_size with no input left
  } else {
    state_ = State::kFailed;  // short of raw_size, or input trailing the final block
  }
}

bool Inflater::emit_byte(std::uint8_t* dst, std::size_t& produced, std::uint8_t b) noexcept {
  if (out_total_ >= raw_size_) {
    state_ = State::kFailed;  // over-long: the stream produces more than raw_size
    return false;
  }
  win_[wpos_] = b;
  wpos_ = (wpos_ + 1u) & (kWindowSize - 1u);
  ++out_total_;
  if (dst != nullptr) {
    dst[produced] = b;  // produced < n: the pump loop's guard, so never past dst + n
  }
  ++produced;
  return true;
}

std::size_t Inflater::pump(std::uint8_t* dst, std::size_t n) noexcept {
  std::size_t produced = 0;
  // Once the output is complete and no match is suspended, keep draining in this same call until the
  // final block's end, so a loader that reads exactly raw_size bytes sees done() right away.
  while (state_ == State::kArmed && (produced < n || (out_total_ == raw_size_ && match_len_ == 0))) {
    if (match_len_ > 0) {
      // Continue a match suspended by a full dst: one byte at a time, from the window ring.
      const std::uint8_t b = win_[(wpos_ - match_dist_) & (kWindowSize - 1u)];
      if (!emit_byte(dst, produced, b)) {
        break;
      }
      --match_len_;
      continue;
    }
    if (!block_open_) {
      if (!start_block()) {
        break;
      }
      continue;
    }
    if (stored_left_ > 0) {
      const int b = bits(8);
      if (b < 0) {
        state_ = State::kFailed;  // truncated inside a stored block
        break;
      }
      --stored_left_;
      if (!emit_byte(dst, produced, static_cast<std::uint8_t>(b))) {
        break;
      }
      continue;
    }
    if (stored_mode_) {
      block_open_ = false;  // a stored block of length 0 ends here
      if (final_) {
        finish();
      }
      continue;
    }
    const int sym = huff_decode(lit_);
    if (sym < 0) {
      state_ = State::kFailed;  // truncated, or a code the table does not have
      break;
    }
    if (sym < 256) {
      if (!emit_byte(dst, produced, static_cast<std::uint8_t>(sym))) {
        break;
      }
      continue;
    }
    if (sym == 256) {
      block_open_ = false;  // end of block
      if (final_) {
        finish();
      }
      continue;
    }
    if (sym > 285) {
      state_ = State::kFailed;  // 286 and 287 never occur in a valid stream
      break;
    }
    const int li = sym - 257;
    int e = bits(kLenExtra[li]);
    if (e < 0) {
      state_ = State::kFailed;
      break;
    }
    const int len = kLenBase[li] + e;
    const int dsym = huff_decode(dist_);
    if (dsym < 0 || dsym > 29) {
      state_ = State::kFailed;  // 30 and 31 never occur in a valid stream
      break;
    }
    e = bits(kDistExtra[dsym]);
    if (e < 0) {
      state_ = State::kFailed;
      break;
    }
    const int dist = kDistBase[dsym] + e;
    if (dist > static_cast<int>(kWindowSize) || static_cast<std::size_t>(dist) > out_total_) {
      state_ = State::kFailed;  // over the 4 KiB window, or reaching before the stream's start
      break;
    }
    match_len_ = static_cast<std::uint16_t>(len);
    match_dist_ = static_cast<std::uint16_t>(dist);
  }
  return produced;
}

}  // namespace o3os::core
