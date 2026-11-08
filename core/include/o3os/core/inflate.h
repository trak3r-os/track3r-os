// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace o3os::core {

// A raw-deflate (RFC 1951) streaming decoder over an input held entirely in memory: stored, fixed and
// dynamic Huffman blocks, written from scratch for OS-FLASHDIET (docs/specs/OS-FLASHDIET.md 3.2) -- no
// zlib, no allocation, no recursion. The 4 KiB window ring and the Huffman tables live in the object
// (about 5 KiB), so its owner places it where it wants (a loader, SDRAM_STATIC, ...).
//
// Pull interface:
//   begin(z, z_size, raw_size)  arm the decoder: z is the whole raw-deflate stream, raw_size the exact
//                               number of bytes it must produce.
//   read(dst, n)                produce up to n bytes into dst and return how many; it suspends only
//                               when dst is full before raw_size bytes are out (in the middle of a
//                               match if need be) or the stream has ended. Never writes past dst + n.
//   skip(n)                     produce up to n bytes and discard them (the window still advances, so a
//                               later read can reach back into skipped data); return how many.
//   done()                      the stream ended exactly at raw_size, every input byte consumed; the
//                               read/skip that produces the last byte also drains the final block's
//                               end in the same call, so done() is true as soon as the output is
//                               complete (unless a match was suspended mid-copy).
//   failed()                    the stream is malformed: truncated, over-long (more output than
//                               raw_size, or input left after the final block), bad block type, bad
//                               Huffman table, a dynamic block with no end-of-block code, or a
//                               distance over 4 KiB or before the stream's start.
//
// Every stream it reads is made by the build with a window of at most 4 KiB (A2 enforces it). Bounded:
// one read produces at most n bytes and never runs past the input. Decode is bit by bit, about 100
// cycles a symbol: fine at boot, measured in A3/A4. Main loop only: never in the render or an ISR.
// Not thread-safe.
class Inflater {
 public:
  Inflater() noexcept = default;

  void begin(const std::uint8_t* z, std::size_t z_size, std::size_t raw_size) noexcept;
  std::size_t read(std::uint8_t* dst, std::size_t n) noexcept;
  std::size_t skip(std::size_t n) noexcept;
  bool done() const noexcept { return state_ == State::kDone; }
  bool failed() const noexcept { return state_ == State::kFailed; }

 private:
  static constexpr std::uint32_t kWindowSize = 4096;

  enum class State : std::uint8_t { kIdle, kArmed, kDone, kFailed };

  // A canonical Huffman table (RFC 1951 3.2.2): how many codes have each length, and the symbols sorted
  // by (length, value), so one code can be decoded bit by bit without a lookup tree.
  template <std::size_t kMaxSymbols>
  struct Huff {
    int per_len[16] = {};                    // how many codes have each length (index 0: no code)
    std::uint16_t ranked[kMaxSymbols] = {};  // the symbols, sorted by (length, value)
  };

  int bits(int need) noexcept;  // need bits, LSB first; -1 when the input is exhausted
  template <std::size_t N>
  bool huff_build(Huff<N>& h, const std::uint8_t* lengths, int n) noexcept;
  template <std::size_t N>
  int huff_decode(const Huff<N>& h) noexcept;  // the symbol, or -1 truncated, -2 no such code
  bool start_block() noexcept;
  bool parse_dynamic() noexcept;
  void build_fixed() noexcept;
  void finish() noexcept;
  bool emit_byte(std::uint8_t* dst, std::size_t& produced, std::uint8_t b) noexcept;
  std::size_t pump(std::uint8_t* dst, std::size_t n) noexcept;  // dst == nullptr discards (skip)

  State state_ = State::kIdle;
  const std::uint8_t* z_ = nullptr;
  std::size_t z_size_ = 0;
  std::size_t z_next_ = 0;  // next input byte to load into bitbuf_
  std::uint32_t bitbuf_ = 0;
  int bitcnt_ = 0;
  std::size_t raw_size_ = 0;
  std::size_t out_total_ = 0;  // bytes produced so far (window and raw_size are checked against this)
  std::uint8_t win_[kWindowSize] = {};
  std::uint32_t wpos_ = 0;  // next window slot
  bool block_open_ = false;
  bool final_ = false;
  bool stored_mode_ = false;
  std::uint16_t stored_left_ = 0;
  std::uint16_t match_len_ = 0;   // a match suspended mid-copy
  std::uint16_t match_dist_ = 0;
  Huff<288> lit_ = {};  // literal/length codes (256 ends a block, 257..285 are match lengths)
  Huff<32> dist_ = {};  // distance codes
  Huff<19> cl_ = {};    // code-length codes of a dynamic block's header
};

}  // namespace o3os::core
