// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "support/modimp_fixture.h"
#include "support/need.h"
#include "support/rng.h"

#include <o3os/formats/mod/module.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace o3os;
using namespace o3os::formats::mod;
using o3os::test::modimp::Bytes;
using o3os::test::modimp::ModCell;
using o3os::test::modimp::ModSpec;
using o3os::test::modimp::S3mCell;
using o3os::test::modimp::S3mInst;
using o3os::test::modimp::S3mSpec;

// The MOD and S3M reader (OS-MODIMP, formats/mod/module.h): every layout, the refusals, the hostile files. The synthetic files come from support/modimp_fixture.h (written from the
// layouts of docs/formats/mod.md); the second opinions are tools/mod_build.py (ctest mod_python) and libopenmpt (ctest modimp_openmpt).

#if defined(__arm__)
constexpr std::uint32_t kThin = 8;   // the target runs at a few MIPS
constexpr unsigned kMutants = 300;
#else
constexpr std::uint32_t kThin = 1;
constexpr unsigned kMutants = 3000;
#endif

namespace {

Module& module() {
  static Module m;  // about 11 KB
  return m;
}

struct Opened {
  Bytes bytes;
  SpanSource src;
  Status status;
  explicit Opened(const Bytes& b) : bytes(b), src(bytes.data(), static_cast<std::uint32_t>(bytes.size())) { status = module().open(src); }
  bool ok() const { return status.ok(); }
  const ModuleInfo& info() const { return module().info(); }
};

ModSpec simple_mod(const char* tag = "M.K.", int channels = 4) {
  ModSpec m;
  m.tag = tag;
  m.channels = channels;
  m.name = "reference";
  m.samples[0].data = test::modimp::sine8(32);
  m.samples[0].name = "sine";
  m.samples[0].loop_len = 16;
  m.samples[1].data = test::modimp::sine8(64);
  m.samples[1].name = "second";
  m.samples[1].volume = 40;
  m.samples[1].finetune = -3;
  m.samples[1].loop_start = 4;
  m.samples[1].loop_len = 8;
  m.orders = {0, 1, 0};
  m.patterns = {{{{0, 0}, test::modimp::note(12, 1, 0x0C, 0x20)}, {{5, channels - 1}, test::modimp::note(35, 2, 0x0E, 0xA3)}}, {{{63, 0}, test::modimp::note(0, 31, 0x0F, 0x06)}}};
  return m;
}

}  // namespace

TEST_CASE("modimp reader: a ProTracker module: header, order list, cells, samples") {
  const Opened o(test::modimp::build(simple_mod()));
  NEED(o.ok());
  const ModuleInfo& g = o.info();
  CHECK(g.kind == Kind::Mod);
  CHECK(g.variant == Variant::Mk);
  CHECK(std::string(g.tag) == "M.K.");
  CHECK(g.channels == 4);
  CHECK(g.order_count == 3);
  CHECK(g.pattern_count == 2);
  CHECK(g.sample_count == 31);
  CHECK(g.restart == 127);
  CHECK(g.speed == 6);
  CHECK(g.tempo == 125);
  CHECK(std::string(g.name) == "reference");
  CHECK(module().order(0) == 0);
  CHECK(module().order(1) == 1);
  CHECK(module().order(2) == 0);
  CHECK(module().order(3) == Module::kSkip);
  Cell cells[caps::kMaxChannels];
  NEED(module().read_line(0, 0, cells).ok());
  CHECK(cells[0].period == 428);
  CHECK(cells[0].note == 13);         // index 12 + 1
  CHECK(!cells[0].off_table);
  CHECK(cells[0].instrument == 1);
  CHECK(cells[0].effect == 0x0C);
  CHECK(cells[0].param == 0x20);
  CHECK(cells[0].volume == 0xFF);
  CHECK(cells[1].period == 0);
  CHECK(cells[1].note == 0);
  NEED(module().read_line(0, 5, cells).ok());
  CHECK(cells[3].note == 36);
  CHECK(cells[3].instrument == 2);
  CHECK(cells[3].effect == 0x0E);
  CHECK(cells[3].param == 0xA3);
  NEED(module().read_line(1, 63, cells).ok());
  CHECK(cells[0].note == 1);
  CHECK(cells[0].instrument == 31);   // the sample number's high nibble lives in byte 0
  CHECK(cells[0].effect == 0x0F);
  CHECK(!module().read_line(2, 0, cells).ok());      // no such pattern
  CHECK(!module().read_line(0, 64, cells).ok());     // no such row
  SampleInfo s;
  NEED(module().sample(0, s).ok());
  CHECK(s.kind == SampleKind::Pcm);
  CHECK(s.frames == 32);
  CHECK(s.volume == 64);
  CHECK(s.finetune == 0);
  CHECK(s.has_loop);
  CHECK(s.loop_start == 0);
  CHECK(s.loop_len == 32);
  CHECK(s.is_signed);
  CHECK(std::string(s.name) == "sine");
  NEED(module().sample(1, s).ok());
  CHECK(s.frames == 64);
  CHECK(s.volume == 40);
  CHECK(s.finetune == -3);
  CHECK(s.loop_start == 8);
  CHECK(s.loop_len == 16);
  NEED(module().sample(2, s).ok());
  CHECK(s.kind == SampleKind::Empty);
  CHECK(!module().sample(31, s).ok());
  // the PCM: signed 8-bit times 256, in pieces
  NEED(module().sample(1, s).ok());
  std::int16_t pcm[64];
  NEED(module().read_pcm(s, 0, 64, pcm).ok());
  const Bytes sine = test::modimp::sine8(64);
  for (unsigned i = 0; i < 64; ++i) CHECK(pcm[i] == static_cast<std::int16_t>(static_cast<std::int8_t>(sine[i])) * 256);
  std::int16_t one;
  NEED(module().read_pcm(s, 63, 1, &one).ok());
  CHECK(one == pcm[63]);
  CHECK(!module().read_pcm(s, 63, 2, pcm).ok());
  CHECK(!module().read_pcm(s, 65, 0, pcm).ok());
  CHECK(module().read_pcm(s, 64, 0, nullptr).ok());
  CHECK(!module().read_pcm(s, 0, 1, nullptr).ok());
}

TEST_CASE("modimp reader: every variant: the channel count and where the cells are") {
  struct V { const char* tag; int channels; Variant variant; };
  const V variants[] = {{"M.K.", 4, Variant::Mk}, {"M!K!", 4, Variant::Mk2}, {"M&K!", 4, Variant::Mk}, {"N.T.", 4, Variant::Nt}, {"FLT4", 4, Variant::Flt4},
                        {"FLT8", 8, Variant::Flt8}, {"1CHN", 1, Variant::Chn}, {"6CHN", 6, Variant::Chn}, {"8CHN", 8, Variant::Chn}, {"9CHN", 9, Variant::Chn},
                        {"10CH", 10, Variant::Chn}, {"16CH", 16, Variant::Chn}, {"32CH", 32, Variant::Chn}};
  for (const V& v : variants) {
    CAPTURE(v.tag);
    ModSpec m;
    m.tag = v.tag;
    m.channels = v.channels;
    m.samples[0].data = test::modimp::sine8(16);
    m.orders = {0};
    for (int c = 0; c < v.channels; ++c) m.patterns[0][{c % 64, c}] = test::modimp::note(c % 36, 1 + c % 31, c % 16, c * 3 & 0xFF);
    const Opened o(test::modimp::build(m));
    NEED(o.ok());
    CHECK(o.info().variant == v.variant);
    CHECK(o.info().channels == v.channels);
    Cell cells[caps::kMaxChannels];
    for (int c = 0; c < v.channels; ++c) {
      NEED(module().read_line(0, static_cast<std::uint32_t>(c % 64), cells).ok());
      CHECK(cells[c].note == (c % 36) + 1);
      CHECK(cells[c].instrument == 1 + c % 31);
      CHECK(cells[c].effect == c % 16);
      CHECK(cells[c].param == ((c * 3) & 0xFF));
    }
  }
}

TEST_CASE("modimp reader: FLT8 stores each pattern as two blocks of four channels, and its order list holds the even numbers") {
  ModSpec m;
  m.tag = "FLT8";
  m.channels = 8;
  m.samples[0].data = test::modimp::sine8(16);
  m.orders = {0, 2, 4};
  m.patterns.assign(3, {});
  for (int p = 0; p < 3; ++p)
    for (int c = 0; c < 8; ++c) m.patterns[static_cast<std::size_t>(p)][{p + 1, c}] = test::modimp::note(c + 8 * p % 28, 1, 0, p * 8 + c);
  const Opened o(test::modimp::build(m));
  NEED(o.ok());
  CHECK(o.info().pattern_count == 3);  // 6 blocks of 4 channels
  CHECK(module().order(0) == 0);
  CHECK(module().order(1) == 1);
  CHECK(module().order(2) == 2);
  Cell cells[caps::kMaxChannels];
  for (int p = 0; p < 3; ++p) {
    NEED(module().read_line(static_cast<std::uint32_t>(p), static_cast<std::uint32_t>(p + 1), cells).ok());
    for (int c = 0; c < 8; ++c) CHECK(cells[c].param == p * 8 + c);
  }
}

TEST_CASE("modimp reader: periods outside the table are snapped to the nearest note and flagged") {
  ModSpec m;
  m.samples[0].data = test::modimp::sine8(8);
  ModCell c;
  c.sample = 1;
  c.period = 430;
  m.patterns[0][{0, 0}] = c;
  c.period = 100;
  m.patterns[0][{1, 0}] = c;
  c.period = 900;
  m.patterns[0][{2, 0}] = c;
  c.period = 0xFFF;
  m.patterns[0][{3, 0}] = c;
  c.period = 453;
  m.patterns[0][{4, 0}] = c;
  const Opened o(test::modimp::build(m));
  NEED(o.ok());
  Cell cells[4];
  NEED(module().read_line(0, 0, cells).ok());
  CHECK(cells[0].note == 13);
  CHECK(cells[0].off_table);
  NEED(module().read_line(0, 1, cells).ok());
  CHECK(cells[0].note == 36);   // the top of the table
  CHECK(cells[0].off_table);
  NEED(module().read_line(0, 2, cells).ok());
  CHECK(cells[0].note == 1);    // the bottom
  CHECK(cells[0].off_table);
  NEED(module().read_line(0, 3, cells).ok());
  CHECK(cells[0].note == 1);
  NEED(module().read_line(0, 4, cells).ok());
  CHECK(cells[0].note == 12);
  CHECK(!cells[0].off_table);
}

TEST_CASE("modimp reader: MOD samples: volume, finetune, loops, short files, names") {
  ModSpec m;
  m.name = "bad\x01name  ";
  m.orders = {0};
  m.samples[0].data = test::modimp::sine8(40);
  m.samples[0].volume = 200;                    // above 64
  m.samples[0].finetune = 7;
  m.samples[0].name = "ok name ";
  m.samples[1].data = test::modimp::sine8(40);
  m.samples[1].loop_start = 5;                  // words
  m.samples[1].loop_len = 100;                  // past the end: clamped
  m.samples[2].data = test::modimp::sine8(40);
  m.samples[2].loop_start = 30;                 // a loop that starts past the end: dropped
  m.samples[2].loop_len = 4;
  m.samples[3].data = test::modimp::sine8(40);
  m.samples[3].loop_start = 0;
  m.samples[3].loop_len = 1;                    // one word: no loop
  m.samples[4].data = {1, 2, 3};                // an odd length is padded to the word
  const Opened o(test::modimp::build(m));
  NEED(o.ok());
  CHECK(std::string(o.info().name) == "bad?name");
  SampleInfo s;
  NEED(module().sample(0, s).ok());
  CHECK(s.volume == 64);
  CHECK(s.finetune == 7);
  CHECK(std::string(s.name) == "ok name");
  CHECK(!s.has_loop);
  NEED(module().sample(1, s).ok());
  CHECK(s.has_loop);
  CHECK(s.loop_start == 10);
  CHECK(s.loop_len == 30);
  CHECK((s.notes & kNoteLoopClamped) != 0);
  NEED(module().sample(2, s).ok());
  CHECK(!s.has_loop);
  CHECK((s.notes & kNoteLoopClamped) != 0);
  NEED(module().sample(3, s).ok());
  CHECK(!s.has_loop);
  NEED(module().sample(4, s).ok());
  CHECK(s.frames == 4);
  // a file that ends inside the sample data: the frames that are there, and a sample with none is empty
  ModSpec t = m;
  t.truncate_by = 30;
  const Opened c(test::modimp::build(t));
  NEED(c.ok());
  std::uint32_t total = 0;
  unsigned trimmed = 0;
  for (unsigned i = 0; i < 31; ++i) {
    NEED(module().sample(i, s).ok());
    total += s.frames;
    trimmed += (s.notes & kNoteTrimmed) != 0 ? 1u : 0u;
  }
  CHECK(trimmed >= 1);
  CHECK(total == 40 * 4 + 4 - 30);
}

TEST_CASE("modimp reader: junk behind the song length: the whole order table counts, unless the file is too short for it") {
  ModSpec m = simple_mod();
  m.junk[100] = 3;   // pattern 3 does not exist in the file...
  Bytes big = test::modimp::build(m);
  const Opened with(big);
  NEED(with.ok());
  CHECK(with.info().pattern_count == 4);  // ... the layout holds four patterns, so the file is the size it is
  // the same song with the junk number but no room for its pattern: the entries the song plays are the count
  ModSpec n = simple_mod();
  Bytes small = test::modimp::build(n);
  small[952 + 100] = 3;
  const Opened without(small);
  NEED(without.ok());
  CHECK(without.info().pattern_count == 2);
  // not even that
  Bytes cut(small.begin(), small.begin() + 1084 + 1024);
  const Opened c(cut);
  CHECK(!c.ok());
  CHECK(c.status.error == Error::Truncated);
}

TEST_CASE("modimp reader: what is refused, and how") {
  auto refused = [](const Bytes& b, Error e) {
    const Opened o(b);
    CHECK(!o.ok());
    CHECK(o.status.error == e);
    CHECK(module().info().kind == Kind::None);
    Cell cells[caps::kMaxChannels];
    CHECK(!module().read_line(0, 0, cells).ok());
    SampleInfo s;
    CHECK(!module().sample(0, s).ok());
  };
  refused(Bytes(), Error::BadMagic);
  refused(Bytes(50, 0), Error::BadMagic);
  refused(Bytes(1083, 0x55), Error::BadMagic);
  refused(Bytes(5000, 0x55), Error::BadMagic);
  // a 15-sample Soundtracker file has no signature: refused as unknown, not guessed
  ModSpec st = simple_mod();
  Bytes b = test::modimp::build(st);
  std::memcpy(&b[1080], "\0\0\0\0", 4);
  refused(b, Error::BadMagic);
  for (const char* tag : {"OCTA", "OKTA", "CD81", "CD61", "TDZ1", "0CHN", "00CH", "33CH", "9CH?", "FLT6", "m.k.", "FA08"}) {
    CAPTURE(tag);
    Bytes t = test::modimp::build(st);
    std::memcpy(&t[1080], tag, 4);
    refused(t, Error::BadMagic);
  }
  Bytes c = test::modimp::build(st);
  c[950] = 0;
  refused(c, Error::Malformed);     // a song length of 0
  c[950] = 129;
  refused(c, Error::Malformed);
  c = test::modimp::build(st);
  c[952 + 3] = 200;
  c[950] = 4;
  refused(c, Error::LimitExceeded);  // a pattern number above 127 in the song
  // the signature that says the file is too short for its patterns
  const Bytes whole = test::modimp::build(st);
  refused(Bytes(whole.begin(), whole.begin() + 1100), Error::Truncated);
}

// ---- S3M ---------------------------------------------------------------------------------------------------------------------------------

namespace {

S3mSpec simple_s3m() {
  S3mSpec m;
  m.name = "reference s3m";
  m.instruments.resize(3);
  m.instruments[0].data = Bytes(32, 0);
  for (std::size_t i = 0; i < 32; ++i) m.instruments[0].data[i] = static_cast<std::uint8_t>(128 + (i % 8) * 10);
  m.instruments[0].name = "unsigned";
  m.instruments[0].loop = true;
  m.instruments[0].loop_start = 4;
  m.instruments[0].loop_end = 20;
  m.instruments[0].c2spd = 22050;
  m.instruments[0].volume = 48;
  m.instruments[1].type = 2;   // AdLib
  m.instruments[1].name = "adlib";
  m.instruments[2].data = Bytes(16, 0);
  m.instruments[2].sixteen = true;
  m.instruments[2].name = "sixteen";
  m.instruments[2].c2spd = 0;
  m.orders = {0, 1};
  S3mCell c;
  c.note = 0x40;
  c.inst = 1;
  c.vol = 64;
  m.patterns = {{{{0, 0}, c}}, {}};
  c.note = 254;
  c.inst = 0;
  c.vol = 255;
  c.cmd = 20;
  c.info = 0x90;
  m.patterns[0][{3, 1}] = c;
  c.note = 0x5C;   // a semitone of 12: the next octave's
  c.inst = 3;
  c.cmd = 0;
  c.info = 0;
  m.patterns[1][{63, 3}] = c;
  return m;
}

}  // namespace

TEST_CASE("modimp reader: a Scream Tracker 3 module: header, channels, orders, cells, instruments") {
  const Opened o(test::modimp::build(simple_s3m()));
  NEED(o.ok());
  const ModuleInfo& g = o.info();
  CHECK(g.kind == Kind::S3m);
  CHECK(g.variant == Variant::S3m);
  CHECK(g.channels == 4);
  CHECK(g.order_count == 2);
  CHECK(g.pattern_count == 2);
  CHECK(g.sample_count == 3);
  CHECK(g.speed == 6);
  CHECK(g.tempo == 125);
  CHECK(g.stereo);
  CHECK(!g.signed_samples);
  CHECK(std::string(g.name) == "reference s3m");
  CHECK(g.chan_used[0]);
  CHECK(g.chan_pan[0] == 20);   // left: 3 of 15
  CHECK(g.chan_pan[1] == 80);   // right: 12 of 15
  CHECK(!g.chan_used[4]);
  Cell cells[caps::kMaxChannels];
  NEED(module().read_line(0, 0, cells).ok());
  CHECK(cells[0].note == 49);       // octave 4 x 12 + 0 + 1
  CHECK(cells[0].instrument == 1);
  CHECK(cells[0].volume == 64);
  CHECK(cells[0].effect == 0);
  NEED(module().read_line(0, 3, cells).ok());
  CHECK(cells[1].cut);
  CHECK(cells[1].note == 0);
  CHECK(cells[1].effect == 20);
  CHECK(cells[1].param == 0x90);
  NEED(module().read_line(1, 63, cells).ok());
  CHECK(cells[3].note == 5 * 12 + 12 + 1);
  CHECK(cells[3].bad_note);
  CHECK(cells[3].instrument == 3);
  NEED(module().read_line(1, 0, cells).ok());   // an empty row
  for (unsigned c = 0; c < 4; ++c) {
    CHECK(cells[c].note == 0);
    CHECK(!cells[c].cut);
    CHECK(cells[c].volume == 255);
    CHECK(cells[c].effect == 0);
  }
  SampleInfo s;
  NEED(module().sample(0, s).ok());
  CHECK(s.kind == SampleKind::Pcm);
  CHECK(s.frames == 32);
  CHECK(s.volume == 48);
  CHECK(s.c2spd == 22050);
  CHECK(s.has_loop);
  CHECK(s.loop_start == 4);
  CHECK(s.loop_len == 16);
  CHECK(!s.sixteen_bit);
  CHECK(!s.is_signed);
  NEED(module().sample(1, s).ok());
  CHECK(s.kind == SampleKind::AdLib);
  NEED(module().sample(2, s).ok());
  CHECK(s.kind == SampleKind::Pcm);
  CHECK(s.sixteen_bit);
  CHECK(s.frames == 8);
  CHECK(s.c2spd == 0);
  // unsigned 8-bit data is re-centred
  NEED(module().sample(0, s).ok());
  std::int16_t pcm[32];
  NEED(module().read_pcm(s, 0, 32, pcm).ok());
  for (unsigned i = 0; i < 32; ++i) CHECK(pcm[i] == (static_cast<int>(i % 8) * 10) * 256);
}

TEST_CASE("modimp reader: S3M orders: 254 is skipped, 255 ends the song, a number with no pattern is skipped and counted") {
  S3mSpec m = simple_s3m();
  m.orders = {1, 254, 0, 9, 255, 0};
  const Opened o(test::modimp::build(m));
  NEED(o.ok());
  CHECK(o.info().order_count == 2);
  CHECK(module().order(0) == 1);
  CHECK(module().order(1) == 0);
  CHECK(o.info().skipped_orders == 1);
  CHECK(o.info().bad_orders == 1);
  S3mSpec e = simple_s3m();
  e.orders = {};
  const Opened none(test::modimp::build(e));
  NEED(none.ok());
  CHECK(none.info().order_count == 0);
}

TEST_CASE("modimp reader: S3M channel settings: muted, AdLib, unused, the panning table") {
  S3mSpec m = simple_s3m();
  m.channels = {0, 8, 0x80 | 3, 17, 255, 5};
  const Opened o(test::modimp::build(m));
  NEED(o.ok());
  const ModuleInfo& g = o.info();
  CHECK(g.channels == 6);
  CHECK(g.chan_used[2]);
  CHECK(g.chan_muted[2]);
  CHECK(g.chan_pan[2] == 20);
  CHECK(g.chan_adlib[3]);
  CHECK(!g.chan_used[4]);
  CHECK(g.chan_used[5]);
  CHECK(g.muted_channels == 1);
  CHECK(g.adlib_channels == 1);
  // mono: the centre everywhere
  S3mSpec mono = m;
  mono.master_volume = 0x30;
  const Opened o2(test::modimp::build(mono));
  NEED(o2.ok());
  CHECK(!o2.info().stereo);
  for (unsigned c = 0; c < 6; ++c) CHECK((!o2.info().chan_used[c] || o2.info().chan_pan[c] == 50));
  // the table: bit 5 says the entry is valid, the low nibble is the pan
  S3mSpec t = simple_s3m();
  t.pan_table.assign(32, 0);
  t.pan_table[0] = 0x20 | 15;
  t.pan_table[1] = 0x20 | 0;
  t.pan_table[2] = 0x20 | 7;
  t.pan_table[3] = 0x05;   // not valid: the default stays
  const Opened o3(test::modimp::build(t));
  NEED(o3.ok());
  CHECK(o3.info().chan_pan[0] == 100);
  CHECK(o3.info().chan_pan[1] == 0);
  CHECK(o3.info().chan_pan[2] == 47);   // 7 of 15
  CHECK(o3.info().chan_pan[3] == 80);
}

TEST_CASE("modimp reader: S3M start values: speed 0 is 6, a tempo below 32 is 32") {
  for (const auto& st : std::vector<std::pair<int, int>>{{6, 125}, {0, 125}, {1, 33}, {255, 255}, {6, 0}, {6, 20}, {6, 31}, {6, 32}}) {
    S3mSpec m = simple_s3m();
    m.speed = st.first;
    m.tempo = st.second;
    const Opened o(test::modimp::build(m));
    NEED(o.ok());
    CHECK(o.info().speed == (st.first == 0 ? 6 : st.first));
    CHECK(o.info().tempo == (st.second < 32 ? 32 : st.second));
  }
}

TEST_CASE("modimp reader: S3M instruments: refused kinds, stereo, signed and unsigned, 16 bit, loops, short data") {
  S3mSpec m;
  m.orders = {0};
  m.instruments.resize(7);
  m.instruments[0].data = Bytes(20, 200);
  m.instruments[0].packed = true;                 // ADPCM
  m.instruments[1].data = Bytes(20, 3);
  m.instruments[1].stereo = true;
  m.instruments[2].data = Bytes(20, 0);
  m.instruments[2].loop = true;
  m.instruments[2].loop_start = 18;
  m.instruments[2].loop_end = 99999;
  m.instruments[3].type = 0;
  m.instruments[4].data = Bytes(20, 0);
  m.instruments[4].loop = true;
  m.instruments[4].loop_start = 30;               // past the end
  m.instruments[4].loop_end = 40;
  m.instruments[5].data = Bytes(20, 0);
  m.instruments[5].c2spd = 999999999;
  m.instruments[6].data = Bytes(20, 0);
  m.instruments[6].volume = 99;
  const Bytes full = test::modimp::build(m);
  const Opened o(full);
  NEED(o.ok());
  SampleInfo s;
  NEED(module().sample(0, s).ok());
  CHECK(s.kind == SampleKind::Unsupported);
  NEED(module().sample(1, s).ok());
  CHECK(s.kind == SampleKind::Pcm);
  CHECK((s.notes & kNoteStereo) != 0);
  CHECK(s.frames == 20);                          // the left channel
  NEED(module().sample(2, s).ok());
  CHECK(s.has_loop);                              // the end is cut to 20: 18..20 is 2 frames, still a loop
  CHECK(s.loop_start == 18);
  CHECK(s.loop_len == 2);
  CHECK((s.notes & kNoteLoopClamped) != 0);
  NEED(module().sample(3, s).ok());
  CHECK(s.kind == SampleKind::Empty);
  NEED(module().sample(4, s).ok());
  CHECK(!s.has_loop);
  CHECK((s.notes & kNoteLoopClamped) != 0);
  NEED(module().sample(5, s).ok());
  CHECK(s.c2spd == 999999999u);
  NEED(module().sample(6, s).ok());
  CHECK(s.volume == 64);
}

TEST_CASE("modimp reader: S3M samples of every format become the same 16-bit PCM") {
  const int values[] = {-128, -100, -1, 0, 1, 100, 127};
  for (const bool is_signed : {true, false}) {
    for (const bool sixteen : {false, true}) {
      S3mSpec m;
      m.is_signed = is_signed;
      m.orders = {0};
      S3mInst in;
      for (int v : values) {
        const int stored = sixteen ? v * 256 : v;
        const int raw = is_signed ? stored : stored + (sixteen ? 32768 : 128);
        in.data.push_back(static_cast<std::uint8_t>(raw & 0xFF));
        if (sixteen) in.data.push_back(static_cast<std::uint8_t>((raw >> 8) & 0xFF));
      }
      in.sixteen = sixteen;
      m.instruments = {in};
      const Opened o(test::modimp::build(m));
      NEED(o.ok());
      SampleInfo s;
      NEED(module().sample(0, s).ok());
      CAPTURE(is_signed);
      CAPTURE(sixteen);
      CHECK(s.frames == 7);
      CHECK(s.is_signed == is_signed);
      std::int16_t pcm[7];
      NEED(module().read_pcm(s, 0, 7, pcm).ok());
      for (unsigned i = 0; i < 7; ++i) CHECK(pcm[i] == values[i] * 256);
    }
  }
}

TEST_CASE("modimp reader: S3M packed patterns: every kind of entry, several to a row, an empty pattern, data that ends early") {
  S3mSpec m;
  m.orders = {0, 1, 2};
  m.channels = {0, 8, 1, 9, 2, 10, 3, 11};
  m.instruments.resize(1);
  m.instruments[0].data = Bytes(8, 0);
  m.patterns.assign(3, {});
  m.empty_patterns = {1};
  for (int ch = 0; ch < 8; ++ch) {
    S3mCell c;
    c.note = 0x30 + ch;
    c.inst = ch + 1;
    c.vol = ch * 8;
    c.cmd = 1 + ch;
    c.info = 0x10 * ch + ch;
    m.patterns[0][{ch * 8, ch}] = c;
  }
  S3mCell vol_only;
  vol_only.vol = 33;
  m.patterns[0][{1, 0}] = vol_only;
  S3mCell fx_only;
  fx_only.cmd = 19;
  fx_only.info = 0xB2;
  m.patterns[0][{1, 1}] = fx_only;
  S3mCell note_only;
  note_only.note = 0x4B;
  m.patterns[0][{1, 2}] = note_only;
  const Opened o(test::modimp::build(m));
  NEED(o.ok());
  Cell cells[caps::kMaxChannels];
  for (int ch = 0; ch < 8; ++ch) {
    NEED(module().read_line(0, static_cast<std::uint32_t>(ch * 8), cells).ok());
    CHECK(cells[ch].note == 3 * 12 + ch + 1);
    CHECK(cells[ch].instrument == ch + 1);
    CHECK(cells[ch].volume == ch * 8);
    CHECK(cells[ch].effect == 1 + ch);
    CHECK(cells[ch].param == 0x10 * ch + ch);
  }
  NEED(module().read_line(0, 1, cells).ok());
  CHECK(cells[0].volume == 33);
  CHECK(cells[0].note == 0);
  CHECK(cells[0].instrument == 0);
  CHECK(cells[1].effect == 19);
  CHECK(cells[1].param == 0xB2);
  CHECK(cells[2].note == 4 * 12 + 11 + 1);
  CHECK(cells[2].volume == 255);
  // the empty pattern (a parapointer of 0): 64 empty rows
  for (std::uint32_t row = 0; row < 64; row += 21) {
    NEED(module().read_line(1, row, cells).ok());
    CHECK(cells[0].note == 0);
    CHECK(cells[0].effect == 0);
  }
  // going back to an earlier pattern and a row twice works (the decoded pattern is a cache)
  NEED(module().read_line(0, 8, cells).ok());
  CHECK(cells[1].instrument == 2);
  NEED(module().read_line(0, 8, cells).ok());
  CHECK(cells[1].instrument == 2);
}

TEST_CASE("modimp reader: S3M rows are decoded to the end of the pattern, and a pattern that is cut short has empty rows") {
  S3mSpec m;
  m.orders = {0};
  m.instruments.resize(1);
  m.instruments[0].data = Bytes(8, 0);
  S3mCell c;
  c.note = 0x40;
  c.inst = 1;
  m.patterns[0][{0, 0}] = c;
  m.patterns[0][{40, 0}] = c;
  Bytes b = test::modimp::build(m);
  // a parapointer is at 96 + 1 (orders) + 2 (instrument); the packed length u16 sits at the pattern
  const std::size_t para = b[96 + 1 + 2] | (static_cast<std::size_t>(b[96 + 1 + 3]) << 8);
  const std::size_t at = para * 16;
  const std::uint32_t len = b[at] | (static_cast<std::uint32_t>(b[at + 1]) << 8);
  b[at] = 10;     // the pattern says it is 10 bytes long: row 0's entry, then the end of row 0, then rows 1-9... and no more
  b[at + 1] = 0;
  CHECK(len > 10);
  const Opened o(b);
  NEED(o.ok());
  Cell cells[caps::kMaxChannels];
  NEED(module().read_line(0, 0, cells).ok());
  CHECK(cells[0].note == 49);
  NEED(module().read_line(0, 40, cells).ok());
  CHECK(cells[0].note == 0);   // past the data: empty
  // a length that runs past the file
  Bytes t = b;
  t[at] = 0xFF;
  t[at + 1] = 0xFF;
  const Opened o2(t);
  NEED(o2.ok());
  CHECK(!module().read_line(0, 0, cells).ok());
  // a length of 0 or 1: not a pattern
  t[at] = 1;
  t[at + 1] = 0;
  const Opened o3(t);
  NEED(o3.ok());
  const Status s = module().read_line(0, 0, cells);
  CHECK(!s.ok());
  CHECK(s.error == Error::Malformed);
}

TEST_CASE("modimp reader: the S3M decoder's work is capped: switching between big patterns row after row ends in LimitExceeded") {
  S3mSpec m;
  m.orders = {0, 1};
  m.channels.assign(32, 0);
  for (int c = 0; c < 32; ++c) m.channels[static_cast<std::size_t>(c)] = c % 16;
  m.instruments.resize(1);
  m.instruments[0].data = Bytes(8, 0);
  m.patterns.assign(2, {});
  for (int p = 0; p < 2; ++p)
    for (int row = 0; row < 64; ++row)
      for (int ch = 0; ch < 32; ++ch) {
        S3mCell c;
        c.note = 0x40 + ch % 12;
        c.inst = 1;
        c.vol = 10;
        c.cmd = 1;
        c.info = 3;
        m.patterns[static_cast<std::size_t>(p)][{row, ch}] = c;
      }
  const Opened o(test::modimp::build(m));
  NEED(o.ok());
  Cell cells[caps::kMaxChannels];
  bool capped = false;
  std::uint64_t reads = 0;
  for (; reads < 200000 && !capped; ++reads) {
    const Status s = module().read_line(static_cast<std::uint32_t>(reads & 1u), 0, cells);
    if (!s.ok()) {
      CHECK(s.error == Error::LimitExceeded);
      capped = true;
    }
  }
  CHECK(capped);
  CHECK(reads > 1000);   // thousands of switches fit: a real song makes a few hundred
}

TEST_CASE("modimp reader: refusals of the S3M header") {
  S3mSpec m = simple_s3m();
  const Bytes good = test::modimp::build(m);
  auto refused = [](const Bytes& b, Error e) {
    const Opened o(b);
    CHECK(!o.ok());
    CHECK(o.status.error == e);
  };
  Bytes b = good;
  b[28] = 0;
  refused(b, Error::BadMagic);       // SCRM, but not ST3's header
  b = good;
  b[29] = 17;
  refused(b, Error::BadMagic);
  b = good;
  b[32] = 0;
  b[33] = 1;                         // 256 orders
  refused(b, Error::LimitExceeded);
  b = good;
  b[36] = 0xFF;
  b[37] = 0xFF;                      // 65,535 patterns
  refused(b, Error::LimitExceeded);
  b = good;
  b[34] = 0xFF;
  b[35] = 0x7F;
  refused(b, Error::LimitExceeded);
  b = good;
  b[34] = 200;
  b[35] = 0;                         // 200 instruments: the tables run past a short file
  Bytes small(b.begin(), b.begin() + 120);
  refused(small, Error::Truncated);
  refused(Bytes(good.begin(), good.begin() + 95), Error::BadMagic);
  // SCRM at 44 of a short file: the header is cut
  Bytes shorty(good.begin(), good.begin() + 97);
  refused(shorty, Error::Truncated);
}

TEST_CASE("modimp reader: sniffing the signature") {
  const Bytes mod = test::modimp::build(simple_mod());
  const Bytes s3m = test::modimp::build(simple_s3m());
  CHECK(sniff_kind(mod.data(), mod.size()) == Kind::Mod);
  CHECK(sniff_kind(mod.data(), 1084) == Kind::Mod);
  CHECK(sniff_kind(mod.data(), 1083) == Kind::None);   // too short to tell
  CHECK(sniff_kind(mod.data(), 16) == Kind::None);
  CHECK(sniff_kind(s3m.data(), s3m.size()) == Kind::S3m);
  CHECK(sniff_kind(s3m.data(), 48) == Kind::S3m);
  CHECK(sniff_kind(s3m.data(), 47) == Kind::None);
  CHECK(sniff_kind(s3m.data(), 0) == Kind::None);
  Bytes junk(1200, 0x41);
  CHECK(sniff_kind(junk.data(), junk.size()) == Kind::None);
  Bytes mmd(1200, 0);
  std::memcpy(mmd.data(), "MMD1", 4);
  CHECK(sniff_kind(mmd.data(), mmd.size()) == Kind::None);
  for (const char* tag : {"M.K.", "M!K!", "M&K!", "N.T.", "FLT4", "FLT8", "6CHN", "16CH", "32CH"}) {
    Bytes b(1100, 0);
    std::memcpy(&b[1080], tag, 4);
    CHECK(sniff_kind(b.data(), b.size()) == Kind::Mod);
  }
  for (const char* tag : {"OCTA", "CD81", "0CHN", "33CH", "FLT6"}) {
    Bytes b(1100, 0);
    std::memcpy(&b[1080], tag, 4);
    CHECK(sniff_kind(b.data(), b.size()) == Kind::None);
  }
}

// ---- hostile files ---------------------------------------------------------------------------------------------------------------------

namespace {

// Everything a reader can be asked, on whatever the bytes are: no crash, no read outside, typed errors only.
void walk(const Bytes& b) {
  const Opened o(b);
  if (!o.ok()) {
    CHECK(o.status.error != Error::None);
    return;
  }
  const ModuleInfo& g = o.info();
  CHECK(g.order_count <= caps::kMaxS3mOrders);
  CHECK(g.channels <= caps::kMaxChannels);
  Cell cells[caps::kMaxChannels];
  for (unsigned i = 0; i < g.order_count; ++i) {
    const std::uint8_t p = module().order(i);
    CHECK((p == Module::kSkip || p < g.pattern_count));
  }
  const unsigned patterns = g.pattern_count < 6 ? g.pattern_count : 6u;
  for (unsigned p = 0; p < patterns; ++p) {
    for (unsigned row = 0; row < 64; row += 9) {
      const Status s = module().read_line(p, row, cells);
      if (!s.ok()) {
        CHECK(s.error != Error::None);
        break;
      }
    }
  }
  const unsigned samples = g.sample_count < 40 ? g.sample_count : 40u;
  for (unsigned i = 0; i < samples; ++i) {
    SampleInfo s;
    const Status st = module().sample(i, s);
    if (st.ok() && s.kind == SampleKind::Pcm) {
      CHECK(static_cast<std::uint64_t>(s.offset) + static_cast<std::uint64_t>(s.frames) * (s.sixteen_bit ? 2u : 1u) <= g.file_size);
      CHECK(s.loop_start + s.loop_len <= s.frames);
      std::int16_t pcm[16];
      const std::uint32_t n = s.frames < 16 ? s.frames : 16u;
      CHECK(module().read_pcm(s, s.frames - n, n, pcm).ok());
    }
  }
}

}  // namespace

TEST_CASE("modimp reader: a file cut at any offset is refused cleanly or read, never worse") {
  const Bytes files[] = {test::modimp::build(simple_mod()), test::modimp::build(simple_mod("FLT8", 8)), test::modimp::build(simple_s3m())};
  for (const Bytes& full : files) {
    unsigned ok = 0, refused = 0;
    for (std::size_t len = 0; len <= full.size(); len += (len < 1200 || len + 200 > full.size()) ? 1 : kThin * 7) {
      const Bytes cut(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(len));
      walk(cut);
      const Opened o(cut);
      (o.ok() ? ok : refused) += 1;
    }
    CHECK(refused > 10);
    CHECK(ok >= 1);
  }
}

TEST_CASE("modimp reader: mutated modules never read outside the file or break the reader") {
  test::Rng rng(0x4D4F44u);
  const Bytes bases[] = {test::modimp::build(simple_mod()), test::modimp::build(simple_mod("8CHN", 8)), test::modimp::build(simple_mod("FLT8", 8)),
                         test::modimp::build(simple_mod("28CH", 28)), test::modimp::build(simple_s3m())};
  for (const Bytes& base : bases) {
    for (unsigned k = 0; k < kMutants; ++k) {
      Bytes m = base;
      const unsigned flips = 1 + rng.below(8);
      for (unsigned i = 0; i < flips; ++i) {
        // bias the damage towards the headers, where the counts and the pointers are
        const std::uint32_t at = rng.below(4) == 0 ? rng.below(static_cast<std::uint32_t>(m.size())) : rng.below(std::min<std::uint32_t>(static_cast<std::uint32_t>(m.size()), 1200u));
        m[at] = rng.byte();
      }
      walk(m);
    }
  }
}

TEST_CASE("modimp reader: random bytes are not a module") {
  test::Rng rng(0xBADF00Du);
  for (unsigned k = 0; k < 200; ++k) {
    Bytes b(rng.below(3000), 0);
    for (auto& x : b) x = rng.byte();
    walk(b);
  }
  // random bytes behind a good signature
  for (unsigned k = 0; k < 200; ++k) {
    Bytes b(1084 + rng.below(4000), 0);
    for (auto& x : b) x = rng.byte();
    std::memcpy(&b[1080], k % 2 ? "M.K." : "8CHN", 4);
    b[950] = static_cast<std::uint8_t>(1 + rng.below(128));
    walk(b);
  }
  for (unsigned k = 0; k < 200; ++k) {
    Bytes b(96 + rng.below(4000), 0);
    for (auto& x : b) x = rng.byte();
    std::memcpy(&b[44], "SCRM", 4);
    b[28] = 0x1A;
    b[29] = 16;
    walk(b);
  }
}

// ---- OS-IMPORT-ANY I6: the 15-sample Soundtracker file ------------------------------------------------------------------------------------------------------------------------------------
// A synthetic one, built here from the layout of a 31-sample file (no third-party file): the title and 15 sample headers, then the song length at 470, the restart byte, 128 orders, and the patterns.

namespace {

// A 31-sample M.K. file with only samples 1..15 in use, as a Soundtracker file: samples 16..31 and the tag are cut out of it.
Bytes soundtracker15(const Bytes& mk) {
  Bytes out(mk.begin(), mk.begin() + 470);
  out.insert(out.end(), mk.begin() + 950, mk.begin() + 1080);
  out.insert(out.end(), mk.begin() + 1084, mk.end());
  out[471] = 120;  // what a Soundtracker file keeps in the restart byte: not a position
  return out;
}

ModSpec soundtracker_spec() {
  ModSpec m = simple_mod();
  m.patterns[1] = {{{63, 0}, test::modimp::note(0, 2, 0x0F, 0x06)}};  // (sample 2: a 15-sample file has no sample 31)
  return m;
}

}  // namespace

TEST_CASE("modimp reader: a 15-sample Soundtracker file reads as a 4-channel module with 15 samples, cells and samples as the 31-sample file's") {
  const Bytes mk = test::modimp::build(soundtracker_spec());
  const Bytes st = soundtracker15(mk);
  CHECK(sniff_kind(st.data(), st.size()) == Kind::None);  // the sniff never claims it
  CHECK(is_soundtracker15(st.data(), st.size() < 1084 ? st.size() : 1084, static_cast<std::uint32_t>(st.size())));
  CHECK_FALSE(is_soundtracker15(mk.data(), 1084, static_cast<std::uint32_t>(mk.size())));  // (the 31-sample one has a tag, and its byte 470 is a name)
  const Opened o(st);
  NEED(o.ok());
  const ModuleInfo& g = o.info();
  CHECK(g.kind == Kind::Mod);
  CHECK(g.variant == Variant::St15);
  CHECK(std::string(g.tag) == "ST15");
  CHECK(g.channels == 4);
  CHECK(g.order_count == 3);
  CHECK(g.pattern_count == 2);
  CHECK(g.sample_count == 15);
  CHECK(g.restart == 127);  // (120 in the file is no position)
  CHECK(std::string(g.name) == "reference");
  CHECK(module().order(0) == 0);
  CHECK(module().order(1) == 1);
  CHECK(module().order(2) == 0);
  Cell cells[caps::kMaxChannels];
  NEED(module().read_line(0, 0, cells).ok());
  CHECK(cells[0].period == 428);
  CHECK(cells[0].instrument == 1);
  CHECK(cells[0].effect == 0x0C);
  CHECK(cells[0].param == 0x20);
  NEED(module().read_line(1, 63, cells).ok());
  CHECK(cells[0].instrument == 2);
  CHECK(cells[0].effect == 0x0F);
  SampleInfo s;
  NEED(module().sample(0, s).ok());
  CHECK(std::string(s.name) == "sine");
  CHECK(s.frames == 32);
  CHECK(s.has_loop);
  std::int16_t pcm[32];
  NEED(module().read_pcm(s, 0, 32, pcm).ok());
  const Bytes sine = test::modimp::sine8(32);
  for (unsigned i = 0; i < 32; ++i) {
    CHECK(pcm[i] == static_cast<std::int16_t>(static_cast<std::int8_t>(sine[i]) * 256));
  }
  NEED(module().sample(1, s).ok());
  CHECK(std::string(s.name) == "second");
  CHECK(s.volume == 40);
  CHECK_FALSE(module().sample(15, s).ok());  // there are 15
  // the 31-sample file is as it was
  const Opened k(mk);
  NEED(k.ok());
  CHECK(k.info().variant == Variant::Mk);
  CHECK(k.info().sample_count == 31);
}

TEST_CASE("modimp reader: a 15-sample file's loop start is in bytes (OS-BUG-ST15LOOP), a 31-sample file's in words") {
  // sample 0 is 32 bytes (16 words); its loop start is the word at 26 of its header, the loop length (words) the one at 28
  const auto loop_of = [](Bytes b, unsigned start, unsigned len_words, SampleInfo& s) {
    b[20 + 26] = static_cast<std::uint8_t>(start >> 8);
    b[20 + 27] = static_cast<std::uint8_t>(start);
    b[20 + 28] = static_cast<std::uint8_t>(len_words >> 8);
    b[20 + 29] = static_cast<std::uint8_t>(len_words);
    const Opened o(b);
    return o.ok() && module().sample(0, s).ok();
  };
  const Bytes st = soundtracker15(test::modimp::build(soundtracker_spec()));
  SampleInfo s;
  // a loop that fits only when the start is read in bytes (20 + 12 <= 32; in words it would end at 52)
  NEED(loop_of(st, 20, 6, s));
  CHECK(s.has_loop);
  CHECK(s.loop_start == 20);
  CHECK(s.loop_len == 12);
  CHECK((s.notes & kNoteLoopClamped) == 0);
  // a loop that fits both ways (4 + 16 <= 32, 8 + 16 <= 32): bytes win
  NEED(loop_of(st, 4, 8, s));
  CHECK(s.has_loop);
  CHECK(s.loop_start == 4);
  CHECK(s.loop_len == 16);
  CHECK((s.notes & kNoteLoopClamped) == 0);
  // (a loop that fits only in words cannot exist: a start that fits in words, 2 * start + length <= size, fits in bytes too)
  // a loop that fits neither way is clamped to the end, as in any module
  NEED(loop_of(st, 24, 6, s));
  CHECK(s.has_loop);
  CHECK(s.loop_start == 24);
  CHECK(s.loop_len == 8);
  CHECK((s.notes & kNoteLoopClamped) != 0);
  // the same words in a 31-sample file stay words: the start 4 is byte 8
  const Bytes mk = test::modimp::build(soundtracker_spec());
  NEED(loop_of(mk, 4, 8, s));
  CHECK(s.loop_start == 8);
  CHECK(s.loop_len == 16);
}

TEST_CASE("modimp reader: Soundtracker edges: length 0, an order of 64, a volume of 65, a name that is not text, a short pattern area, a short last sample, a short earlier one") {
  const Bytes st = soundtracker15(test::modimp::build(soundtracker_spec()));
  auto refused = [](const Bytes& b) {
    const Opened o(b);
    CHECK_FALSE(o.ok());
    CHECK(o.status.error == Error::BadMagic);
    CHECK(module().info().kind == Kind::None);
  };
  Bytes b = st;
  b[470] = 0;  // song length 0
  refused(b);
  b = st;
  b[470] = 129;
  refused(b);
  b = st;
  b[472 + 1] = 64;  // a played order of 64 (patterns 0..63 only)
  refused(b);
  b = st;
  b[20 + 25] = 65;  // the first sample's volume
  refused(b);
  b = st;
  b[3] = 0x07;  // the title is not text
  refused(b);
  b = st;
  b[20 + 30 * 14 + 3] = 0xE9;  // the last sample header's name
  refused(b);
  // an order past the song length that is 64 or more is junk, not a refusal: the patterns the played orders name are all that count
  b = st;
  b[472 + 5] = 99;
  CHECK(Opened(b).ok());
  CHECK(module().info().pattern_count == 2);
  // the file ends inside the patterns: not a Soundtracker file
  refused(Bytes(st.begin(), st.begin() + 600 + 1024));
  refused(Bytes(st.begin(), st.begin() + 600));
  // the last sample may be short (trimmed, flagged); an earlier one may not
  const Bytes data_end(st.begin(), st.end() - 10);  // the second sample is the last with data: 10 bytes of its 64 are gone
  const Opened trimmed(data_end);
  NEED(trimmed.ok());
  SampleInfo s;
  NEED(module().sample(1, s).ok());
  CHECK(s.frames == 54);
  CHECK((s.notes & kNoteTrimmed) != 0);
  refused(Bytes(st.begin(), st.begin() + 600 + 2 * 1024 + 20));  // the first sample (32 bytes) is cut to 20 and nothing follows it
  // a 31-sample file with an unknown tag is still refused as before
  Bytes octa = test::modimp::build(soundtracker_spec());
  std::memcpy(&octa[1080], "OCTA", 4);
  const Opened o(octa);
  CHECK_FALSE(o.ok());
  CHECK(o.status.error == Error::BadMagic);
  // ... also when its 16th sample's name is spaces and the file is long: the old reading took the first space as a song length of 32, the rest as orders of 32 (33 patterns fit)
  Bytes flt6 = test::modimp::build(soundtracker_spec());
  std::memcpy(&flt6[1080], "FLT6", 4);
  std::fill(flt6.begin() + 470, flt6.begin() + 492, std::uint8_t{0x20});  // the 16th sample's name
  std::fill(flt6.begin() + 492, flt6.begin() + 504, std::uint8_t{0});     // its length, finetune, volume, loop; the 17th's name begins
  flt6.resize(600 + 33 * 1024 + 64, 0);
  CHECK_FALSE(is_soundtracker15(flt6.data(), 1084, static_cast<std::uint32_t>(flt6.size())));
  const Opened f(flt6);
  CHECK_FALSE(f.ok());
  CHECK(f.status.error == Error::BadMagic);
  // a loop that starts past the end of its sample is no sample header
  b = st;
  b[20 + 26] = 0x40;  // the first sample (16 words): a loop start of 0x4000 words, a loop length of 16
  refused(b);
}

TEST_CASE("modimp reader: a Soundtracker file with a junk order past the song length counts the patterns the song plays, as the structure check does, however long the file") {
  // a file long enough (over 100 KB, trailing bytes) to hold the 100 patterns the junk order 99 would name: the check and the reader must agree on the 2 patterns the song plays (orders 0, 1, 0),
  // or the samples are read from the wrong place without an error
  Bytes big = soundtracker15(test::modimp::build(soundtracker_spec()));
  big[472 + 5] = 99;  // an order past the song length (3)
  big.resize(600 + 100 * 1024 + 64, 0);
  const auto size = static_cast<std::uint32_t>(big.size());
  CHECK(is_soundtracker15(big.data(), 1084, size));
  const Opened o(big);
  NEED(o.ok());
  CHECK(o.info().variant == Variant::St15);
  CHECK(o.info().pattern_count == 2);
  SampleInfo s;
  NEED(module().sample(0, s).ok());
  CHECK(std::string(s.name) == "sine");
  CHECK(s.frames == 32);
  std::int16_t pcm[32];  // the data itself: read from where the 2 patterns end
  NEED(module().read_pcm(s, 0, 32, pcm).ok());
  const Bytes sine = test::modimp::sine8(32);
  for (unsigned i = 0; i < 32; ++i) {
    CHECK(pcm[i] == static_cast<std::int16_t>(static_cast<std::int8_t>(sine[i]) * 256));
  }
}
