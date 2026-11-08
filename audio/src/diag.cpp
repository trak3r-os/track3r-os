// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/audio/diag.h>

#include <o3os/core/fast_math.h>

namespace o3os::audio::diag {

Text::Text(char* buf, std::size_t cap) noexcept : buf_(buf), cap_(cap) {
  if (cap_ != 0) {
    buf_[0] = '\0';
  }
}

Text& Text::chr(char c) noexcept {
  if (cap_ == 0) {
    return *this;
  }
  if (len_ + 1 < cap_) {
    buf_[len_++] = c;
    buf_[len_] = '\0';
  } else {
    truncated_ = true;
  }
  return *this;
}

Text& Text::str(const char* s) noexcept {
  for (; *s != '\0'; ++s) {
    chr(*s);
  }
  return *this;
}

Text& Text::dec(std::uint32_t v) noexcept {
  char digits[10];
  unsigned n = 0;
  do {
    digits[n++] = static_cast<char>('0' + v % 10u);
    v /= 10u;
  } while (v != 0u);
  while (n != 0u) {
    chr(digits[--n]);
  }
  return *this;
}

Text& Text::hex32(std::uint32_t v) noexcept {
  str("0x");
  for (int shift = 28; shift >= 0; shift -= 4) {
    chr("0123456789ABCDEF"[(v >> shift) & 0xFu]);
  }
  return *this;
}

Text& Text::percent(std::uint64_t part, std::uint64_t whole) noexcept {
  if (whole == 0u) {
    return str("-");
  }
  // One decimal of a percent needs only the leading bits of the ratio. Both are scaled down (shifts: no division) until each is below 2^22,
  // so that part * 1000 fits 32 bits and the division below is the Cortex-M4's UDIV, not the 64-bit helper (about 800 bytes of flash that
  // nothing else in the image links). Below 2^22, which is every cycle count of a block, the result is exact and unchanged.
  while (part > 0x3FFFFFu || whole > 0x3FFFFFu) {
    part >>= 1;
    whole >>= 1;
  }
  const std::uint32_t w = whole != 0u ? static_cast<std::uint32_t>(whole) : 1u;
  const std::uint32_t tenths = static_cast<std::uint32_t>(part) * 1000u / w;  // permille: one decimal of a percent
  dec(tenths / 10u);
  chr('.');
  dec(tenths % 10u);
  return str(" %");
}

Text& Text::kib(std::uint32_t bytes) noexcept {
  if (bytes % 1024u == 0u) {
    return dec(bytes / 1024u).str(" KiB");
  }
  return dec(bytes).str(" B");
}

namespace {

// "last/max" of one stage.
void stage(Text& t, const char* name, std::uint32_t last, std::uint32_t max) noexcept {
  t.str("  ").str(name).chr(' ').dec(last).chr('/').dec(max);
}

}  // namespace

std::size_t format_cpu(const StatusSnapshot& s, std::uint32_t cycles_per_block, char* out, std::size_t cap) noexcept {
  Text t(out, cap);
  t.str("audio  blocks ").dec(s.boundaries).str("  renders ").dec(s.renders);
  t.str("  underrun ").dec(s.underruns).str(" (run ").dec(s.consecutive_underruns).str(", max ").dec(s.max_consecutive_underruns).chr(')');
  t.str("  mute ").dec(s.muted).str(" (events ").dec(s.mute_events).chr(')');
  t.str("  late ").dec(s.late_drops).str("  slips ").dec(s.slot_slips).str("  hwfault ").dec(s.hw_faults).nl();

  t.str("render last ").dec(s.render_last).str("  max ").dec(s.render_max).str(" cyc = ").percent(s.render_max, cycles_per_block);
  t.str(" of the block (").dec(cycles_per_block).str(" cyc)  over-budget ").dec(s.governor_over).nl();

  t.str("stage ");
  static constexpr const char* kNames[kStageCount] = {"input", "drain", "render", "strips", "buses", "master", "convert", "taps", "commit"};
  static_assert(kStageCount == 9, "one name per stage (hal/audio.h kStage*)");
  for (unsigned i = 0; i < kStageCount; ++i) {
    stage(t, kNames[i], s.stage_last[i], s.stage_max[i]);
  }
  t.str("  (last/max cyc)").nl();

  t.str("jitter start-latency ").dec(s.latency_last).chr('/').dec(s.latency_max).str(" cyc  boundary period ");
  t.dec(s.period_min).str("..").dec(s.period_max).str(" cyc  input-peak ").dec(s.input_peak).nl();
  return t.size();
}

std::size_t format_shed(std::uint32_t blocks, std::uint32_t late, std::uint32_t runs, std::uint32_t longest, std::uint32_t window, char* out, std::size_t cap) noexcept {
  Text t(out, cap);
  t.str("guard  shed ").dec(blocks).str(" blocks in ").dec(runs).str(" runs  late ").dec(late).str("  longest run since boot ").dec(longest).str(" blocks  n ").dec(window).nl();
  return t.size();
}

std::size_t format_wt_bake(std::uint32_t used, std::uint32_t capacity, std::uint32_t refused, char* out, std::size_t cap) noexcept {
  Text t(out, cap);
  t.str("wtbake  used ").dec(used).chr('/').dec(capacity).str("  refused ").dec(refused).str(" calls");
  return t.size();
}

std::size_t format_voices(std::uint32_t in_use, std::uint32_t pool, std::uint32_t peak, std::uint32_t steals, std::uint32_t advanced, char* out, std::size_t cap) noexcept {
  Text t(out, cap);
  t.str("voices  in use ").dec(in_use).chr('/').dec(pool).str("  peak ").dec(peak).str("  steals ").dec(steals).str("  advanced ").dec(advanced).nl();
  return t.size();
}

std::size_t format_app_tap(unsigned source, std::uint32_t blocks, std::uint32_t last, std::uint32_t sum, std::uint32_t max, std::uint32_t cpu_hz, char* out, std::size_t cap) noexcept {
  Text t(out, cap);
  t.str("tap: ").str(source == 1u ? "input" : (source == 2u ? "master" : "off"));
  if (blocks == 0u) {
    t.str(", no block timed");
    return t.size();
  }
  t.str(", ").dec(blocks).str(" blocks, write cycles last ").dec(last).str(" mean ").dec(sum / blocks).str(" max ").dec(max);
  const std::uint32_t tenths = cpu_hz >= 1000000u ? static_cast<std::uint32_t>(static_cast<std::uint64_t>(max) * 10u / (cpu_hz / 1000000u)) : 0u;  // tenths of a microsecond
  t.str(" (max ").dec(tenths / 10u).chr('.').dec(tenths % 10u).str(" us)");
  return t.size();
}

std::size_t format_module_perf(unsigned instrument, const ModulePerf& p, char* out, std::size_t cap) noexcept {
  Text t(out, cap);
  t.str("mod: perf ").dec(instrument).str(": ");
  if (p.blocks == 0u) {
    t.str("no full block measured");
    return t.size();
  }
  t.dec(p.blocks).str(" blocks, loop min ").dec(p.loop_min).str(" mean ").dec(p.loop_mean()).str(" max ").dec(p.loop_max);
  t.str(", render min ").dec(p.render_min).str(" mean ").dec(p.render_mean()).str(" max ").dec(p.render_max);
  t.str(", out mean ").dec(p.out_mean()).str(", hooks ").dec(p.hooks_mean());
  return t.size();
}

namespace {

// A linear amplitude (1.0 = full scale) as dBFS with one decimal, 6 characters wide ("-12.3", "  0.0", " -inf").
void dbfs(Text& t, float amplitude) noexcept {
  if (!(amplitude > 0.00001f)) {  // below -100 dB, and a NaN
    t.str(" -inf");
    return;
  }
  const float db = 6.0205999f * core::log2_fast(amplitude);
  int tenths = static_cast<int>(db * 10.0f + (db >= 0.0f ? 0.5f : -0.5f));
  const bool neg = tenths < 0;
  tenths = neg ? -tenths : tenths;
  if (tenths >= 1000) {  // a mixer never shows more than the +99.9 a text of this width can
    tenths = 999;
  }
  const std::uint32_t whole = static_cast<std::uint32_t>(tenths) / 10u;
  t.chr(whole < 10u ? ' ' : (neg ? '-' : '+'));
  if (whole < 10u) {
    t.chr(neg ? '-' : ' ');
  }
  t.dec(whole).chr('.').dec(static_cast<std::uint32_t>(tenths) % 10u);
}

void channel_name(Text& t, unsigned c) noexcept {
  if (c < 16u) {
    t.chr('T').chr(static_cast<char>('0' + (c + 1u) / 10u)).chr(static_cast<char>('0' + (c + 1u) % 10u));
    return;
  }
  static constexpr const char* kOthers[meter_ch::kCount - 16u] = {"IN L", "IN R", "MAIN L", "MAIN R", "CUE L", "CUE R", "SNDA", "SNDB", "REC L", "REC R", "MST L", "MST R"};
  t.str(kOthers[c - 16u]);
}

}  // namespace

namespace {

// A gain (1.0 = no reduction) as the reduction in dB, positive, one decimal, no padding ("0.0", "6.2", "99.9" at most): the limiter line's numbers.
void reduction_db(Text& t, std::uint32_t gain_q16) noexcept {
  std::uint32_t tenths = 0;
  if (gain_q16 < 65536u) {
    const float gain = static_cast<float>(gain_q16) * (1.0f / 65536.0f);
    const float db = gain > 0.00001f ? -6.0205999f * core::log2_fast(gain) : 99.9f;
    tenths = static_cast<std::uint32_t>(db * 10.0f + 0.5f);
    tenths = tenths > 999u ? 999u : tenths;
  }
  t.dec(tenths / 10u).chr('.').dec(tenths % 10u);
}

std::size_t mix_text(const MeterSnapshot& s, const LimiterReport* limiter, char* out, std::size_t cap) noexcept {
  Text t(out, cap);
  t.str("mix  windows ").dec(s.windows).str("  frames ").dec(s.frames).str("  (dBFS; clip = windows at full scale)").nl();
  if (limiter != nullptr) {  // (after the header, before anything that may end the text early: the line is always there)
    t.str("limiter: gr ");
    reduction_db(t, limiter->gain_now_q16);
    t.str(" dB max ");
    reduction_db(t, limiter->gain_hold_q16);
    t.str(" dB, mode ").str(limiter->mode != 0u ? "hard" : "subtle").str(", ceiling -0.3 dBFS").nl();
  }
  if (s.windows == 0u) {
    t.str("  nothing measured yet").nl();
    return t.size();
  }
  for (unsigned c = 0; c < kMeterChannels; ++c) {
    const bool master = c == meter_ch::kMasterL || c == meter_ch::kMasterR;
    if (!master && s.ch[c].hold == 0u && s.ch[c].clips == 0u) {
      continue;
    }
    const MeterLevel level = meter_level(s, c);
    t.str("  ");
    channel_name(t, c);
    t.str("  pk ");
    dbfs(t, level.peak);
    t.str("  hold ");
    dbfs(t, level.hold);
    t.str("  rms ");
    dbfs(t, level.rms);
    t.str("  clip ").dec(s.ch[c].clips).nl();
  }
  return t.size();
}

}  // namespace

std::size_t format_mix(const MeterSnapshot& s, char* out, std::size_t cap) noexcept { return mix_text(s, nullptr, out, cap); }
std::size_t format_mix(const MeterSnapshot& s, const LimiterReport& limiter, char* out, std::size_t cap) noexcept { return mix_text(s, &limiter, out, cap); }

std::size_t format_mem(const MemReport& m, char* out, std::size_t cap) noexcept {
  Text t(out, cap);
  std::uint64_t sdram_sum = 0;
  t.str("sdram ").kib(m.sdram_total).nl();
  for (unsigned i = 0; i < m.sdram_count; ++i) {
    t.str("  ").str(m.sdram[i].name).chr(' ').hex32(m.sdram[i].base).chr(' ').kib(m.sdram[i].size).nl();
    sdram_sum += m.sdram[i].size;
  }
  t.str("  regions sum ").dec(static_cast<std::uint32_t>(sdram_sum / 1024u)).str(" KiB").str(sdram_sum == m.sdram_total ? " (complete)" : " (MISMATCH)").nl();

  t.str("sram_l ").kib(m.sram_l_size).str(": hot code ").dec(m.sram_l_hot_used).str(" B of ").kib(m.sram_l_hot_budget);
  t.str(", tap data ").dec(m.sram_l_data_used).str(" B of ").kib(m.sram_l_data_budget).nl();
  for (unsigned i = 0; i < m.sram_l_count; ++i) {
    t.str("  budget ").str(m.sram_l[i].name).chr(' ').kib(m.sram_l[i].bytes).nl();
  }

  const std::uint32_t free_bytes = m.sram_u_size > m.sram_u_used + m.sram_u_stack ? m.sram_u_size - m.sram_u_used - m.sram_u_stack : 0u;
  t.str("sram_u ").kib(m.sram_u_size).str(": image ").dec(m.sram_u_used).str(" B, stack ").kib(m.sram_u_stack);
  t.str(", free ").dec(free_bytes).str(" B (at least ").kib(m.sram_u_min_free).str(" wanted)").nl();
  std::uint64_t budget_sum = 0;
  for (unsigned i = 0; i < m.sram_u_count; ++i) {
    t.str("  budget ").str(m.sram_u[i].name).chr(' ').kib(m.sram_u[i].bytes).nl();
    budget_sum += m.sram_u[i].bytes;
  }
  t.str("  budget sum ").dec(static_cast<std::uint32_t>(budget_sum)).str(" B").nl();
  return t.size();
}

}  // namespace o3os::audio::diag
