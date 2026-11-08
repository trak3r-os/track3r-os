// SPDX-License-Identifier: GPL-3.0-or-later
//
// hal::Display::upload(): the contract of hal/display.h (size cap, RAM_G bounds, word alignment, ordering against
// frames), checked on the host fake that counts uploads (and records them when asked) and on the simulator's display
// that writes them into its RAM_G; and the font, which the simulator uploads through that contract at start-up.
#include <doctest/doctest.h>

#include <o3os/eve/display_list_builder.h>
#include <o3os/eve/dl.h>
#include <o3os/hal/display.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/sim/eve_raster.h>
#include <o3os/sim/raster_display.h>
#include <o3os/ui/assets.h>
#include <o3os/ui/font.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dl = o3os::eve::dl;
using o3os::eve::BitmapFormat;
using o3os::eve::Filter;
using o3os::eve::Primitive;
using o3os::eve::Wrap;
using o3os::hal::kMaxUploadBytes;
using o3os::hal::kRamGBytes;
using o3os::hal::kUploadAlign;
using o3os::hal::UploadResult;
using o3os::platform::host::FakeDisplay;
using o3os::sim::EveRaster;
using o3os::sim::RasterDisplay;

namespace {

// Upload staging: a buffer that is read with word loads says so. The array type of an alignas'd variable would not
// carry the alignment, so it sits in a struct whose own alignment the static_assert can see. Big enough for three
// chunks of kMaxUploadBytes.
struct alignas(8) Staging {
  std::uint8_t bytes[kMaxUploadBytes * 2 + 1808];
};
static_assert(alignof(Staging) >= kUploadAlign, "staging must satisfy the upload contract");
static_assert(kUploadAlign == 4, "the contract is word alignment");
static_assert(kRamGBytes == 1024u * 1024u, "RAM_G is 1024 KB (guide Memory Map, p.253)");
static_assert(kMaxUploadBytes == 4096 && kMaxUploadBytes % kUploadAlign == 0, "ADR 0010: 4 KB per upload");

Staging make_staging() {
  Staging s{};
  for (std::size_t i = 0; i < sizeof(s.bytes); ++i) {
    s.bytes[i] = static_cast<std::uint8_t>(0xA0 + i);
  }
  return s;
}

constexpr std::size_t kPlenty = 1u << 20;  // a recording cap that nothing here reaches

}  // namespace

TEST_CASE("upload: the fake records each accepted upload with the frames committed before it, when asked to") {
  FakeDisplay display;
  display.record_uploads(kPlenty);
  o3os::hal::Display& sink = display;
  const Staging data = make_staging();

  CHECK(sink.upload(0, data.bytes, 16) == UploadResult::Ok);
  const std::uint32_t frame[] = {dl::display()};
  CHECK(sink.begin_frame());
  CHECK(sink.write(frame, 1));
  sink.end_frame();
  CHECK(sink.upload(4096, data.bytes + 8, 24) == UploadResult::Ok);  // after one committed frame
  CHECK(sink.begin_frame());
  CHECK(sink.upload(8192, data.bytes, 4) == UploadResult::Ok);       // inside an open frame, which is allowed ...
  sink.abort_frame();                                                // ... and the abort does not take it back

  CHECK(display.uploads().size() == 3u);
  if (display.uploads().size() != 3u) {
    return;
  }
  CHECK(display.uploads()[0].addr == 0u);
  CHECK(display.uploads()[0].frames_before == 0u);
  CHECK(display.uploads()[0].bytes.size() == 16u);
  CHECK(std::memcmp(display.uploads()[0].bytes.data(), data.bytes, 16) == 0);
  CHECK(display.uploads()[1].addr == 4096u);
  CHECK(display.uploads()[1].frames_before == 1u);  // it precedes frame number 2
  CHECK(std::memcmp(display.uploads()[1].bytes.data(), data.bytes + 8, 24) == 0);
  CHECK(display.uploads()[2].frames_before == 1u);
  CHECK(display.rejected_uploads() == 0u);
  CHECK(display.accepted_uploads() == 3u);
  CHECK(display.uploaded_bytes() == 16u + 24u + 4u);
  CHECK(display.aborted() == 1u);
}

TEST_CASE("upload: nothing is kept unless asked, and what is kept is capped, so a long run cannot grow without bound") {
  const Staging data = make_staging();
  FakeDisplay quiet;  // the default: counters only
  for (int i = 0; i < 1000; ++i) {
    CHECK(quiet.upload(0, data.bytes, kMaxUploadBytes) == UploadResult::Ok);
  }
  CHECK(quiet.uploads().empty());
  CHECK(quiet.accepted_uploads() == 1000u);
  CHECK(quiet.uploaded_bytes() == 1000u * kMaxUploadBytes);
  CHECK(quiet.unrecorded_uploads() == 0u);  // nothing was asked for, so nothing was left out

  // A cap of 100 bytes: the uploads that fit in order are kept, later ones are counted but not kept.
  FakeDisplay capped;
  capped.record_uploads(100);
  CHECK(capped.upload(0, data.bytes, 60) == UploadResult::Ok);
  CHECK(capped.upload(64, data.bytes, 60) == UploadResult::Ok);  // 120 > 100: taken, not kept
  CHECK(capped.upload(128, data.bytes, 40) == UploadResult::Ok);  // 60 + 40 fits
  CHECK(capped.upload(192, data.bytes, 4) == UploadResult::Ok);   // 100 + 4 does not
  CHECK(capped.upload(256, data.bytes, 0) == UploadResult::Ok);   // count 0 is never kept
  CHECK(capped.uploads().size() == 2u);
  CHECK(capped.uploads()[0].addr == 0u);
  CHECK(capped.uploads()[1].addr == 128u);
  CHECK(capped.accepted_uploads() == 5u);
  CHECK(capped.unrecorded_uploads() == 2u);

  // The simulator's display is bounded the same way: nothing is kept by default.
  EveRaster raster;
  RasterDisplay display(raster);
  for (int i = 0; i < 1000; ++i) {
    CHECK(display.upload(0, data.bytes, kMaxUploadBytes) == UploadResult::Ok);
  }
  CHECK(display.uploads().empty());
  CHECK(display.accepted_uploads() == 1000u);
  display.record_uploads(kPlenty);
  CHECK(display.upload(64, data.bytes, 8) == UploadResult::Ok);
  CHECK(display.uploads().size() == 1u);
}

TEST_CASE("upload: the contract answers with the reason: too large, no data, misaligned, out of RAM_G, busy") {
  FakeDisplay display;
  o3os::hal::Display& sink = display;
  const Staging data = make_staging();
  std::size_t rejected = 0;
  const auto refuse = [&](std::uint32_t addr, const std::uint8_t* bytes, std::size_t count, UploadResult why) {
    CHECK(sink.upload(addr, bytes, count) == why);
    CHECK(display.last_upload_result() == why);
    ++rejected;
    CHECK(display.rejected_uploads() == rejected);
    CHECK(display.accepted_uploads() == 0u);
  };
  // Size: kMaxUploadBytes is the cap, one over is refused (and is its own error), not split.
  refuse(0, data.bytes, kMaxUploadBytes + 1, UploadResult::TooLarge);
  refuse(0, data.bytes, kMaxUploadBytes * 2, UploadResult::TooLarge);
  refuse(0, data.bytes, kRamGBytes, UploadResult::TooLarge);
  // Bounds: the last byte of RAM_G is address 0xFFFFF.
  refuse(kRamGBytes - 8, data.bytes, 9, UploadResult::BadRange);   // one byte too many
  refuse(kRamGBytes, data.bytes, 1, UploadResult::BadRange);       // starts at the end
  refuse(kRamGBytes + 4, data.bytes, 4, UploadResult::BadRange);   // starts beyond it
  refuse(0xFFFFFFFCu, data.bytes, 8, UploadResult::BadRange);      // addr + count would wrap past 2^32: still refused
  // Alignment: both the RAM_G address and the source pointer are multiples of kUploadAlign.
  refuse(2, data.bytes, 8, UploadResult::Misaligned);
  refuse(1, data.bytes, 8, UploadResult::Misaligned);
  refuse(0, data.bytes + 1, 8, UploadResult::Misaligned);
  refuse(0, data.bytes + 2, 8, UploadResult::Misaligned);
  refuse(0, data.bytes + 3, 8, UploadResult::Misaligned);
  refuse(3, data.bytes + 1, 8, UploadResult::Misaligned);
  // No data to read.
  refuse(0, nullptr, 4, UploadResult::NoData);
  // When several reasons apply the first of: TooLarge, NoData, Misaligned, BadRange, Busy.
  display.set_busy(true);
  refuse(kRamGBytes + 4, data.bytes + 1, kMaxUploadBytes + 1, UploadResult::TooLarge);
  refuse(kRamGBytes + 4, nullptr, 4, UploadResult::NoData);
  refuse(kRamGBytes + 2, data.bytes, 4, UploadResult::Misaligned);
  refuse(kRamGBytes, data.bytes, 4, UploadResult::BadRange);
  // A busy transport refuses a valid upload too, and the caller may try again.
  refuse(0, data.bytes, 4, UploadResult::Busy);
  display.set_busy(false);
  CHECK(sink.upload(0, data.bytes, 4) == UploadResult::Ok);
  CHECK(display.last_upload_result() == UploadResult::Ok);
  CHECK(display.accepted_uploads() == 1u);

  // Exactly the cap and exactly the end of RAM_G are fine.
  CHECK(sink.upload(0, data.bytes, kMaxUploadBytes) == UploadResult::Ok);
  CHECK(sink.upload(kRamGBytes - 8, data.bytes, 8) == UploadResult::Ok);
  // A tail of 1..3 bytes is fine (the count is not an alignment), and so is a zero-length upload at an aligned
  // address, up to and including the end of RAM_G; with a null pointer too. The address is still checked.
  CHECK(sink.upload(64, data.bytes, 5) == UploadResult::Ok);
  CHECK(sink.upload(128, data.bytes, 0) == UploadResult::Ok);
  CHECK(sink.upload(kRamGBytes, nullptr, 0) == UploadResult::Ok);
  CHECK(sink.upload(kRamGBytes + 4, nullptr, 0) == UploadResult::BadRange);
  CHECK(sink.upload(2, nullptr, 0) == UploadResult::Misaligned);
}

TEST_CASE("upload: the fake keeps its own copy, the caller's buffer is free at once (ownership)") {
  FakeDisplay display;
  display.record_uploads(kPlenty);
  Staging data = make_staging();
  CHECK(display.upload(0, data.bytes, 8) == UploadResult::Ok);
  std::memset(data.bytes, 0, sizeof(data.bytes));
  CHECK(display.uploads().size() == 1u);
  if (display.uploads().size() != 1u) {
    return;
  }
  CHECK(display.uploads()[0].bytes[0] == 0xA0);
  CHECK(display.uploads()[0].bytes[7] == 0xA7);
}

TEST_CASE("upload: big data goes in chunks of the cap, at word-aligned offsets, and stops at the first refusal") {
  const Staging data = make_staging();
  const std::size_t count = kMaxUploadBytes * 2 + 1808;  // three chunks: 4096, 4096, 1808
  FakeDisplay display;
  display.record_uploads(kPlenty);
  CHECK(o3os::sim::upload_chunked(display, 256, data.bytes, count) == UploadResult::Ok);
  CHECK(display.uploads().size() == 3u);
  if (display.uploads().size() != 3u) {
    return;
  }
  CHECK(display.uploads()[0].addr == 256u);
  CHECK(display.uploads()[1].addr == 256u + kMaxUploadBytes);
  CHECK(display.uploads()[2].addr == 256u + 2 * kMaxUploadBytes);
  CHECK(display.uploads()[0].bytes.size() == kMaxUploadBytes);
  CHECK(display.uploads()[2].bytes.size() == 1808u);
  CHECK(std::memcmp(display.uploads()[2].bytes.data(), data.bytes + 2 * kMaxUploadBytes, 1808) == 0);
  // Nothing to send is Ok, and sends nothing.
  CHECK(o3os::sim::upload_chunked(display, 0, nullptr, 0) == UploadResult::Ok);
  CHECK(display.accepted_uploads() == 3u);
  // A chunk the display refuses ends it: here the third runs past the end of RAM_G; the first two stay uploaded.
  FakeDisplay edge;
  edge.record_uploads(kPlenty);
  CHECK(o3os::sim::upload_chunked(edge, kRamGBytes - kMaxUploadBytes - 16, data.bytes, count) == UploadResult::BadRange);
  CHECK(edge.accepted_uploads() == 1u);
  CHECK(edge.rejected_uploads() == 1u);
  // A misaligned source is refused at the first chunk.
  FakeDisplay misaligned;
  CHECK(o3os::sim::upload_chunked(misaligned, 0, data.bytes + 1, 64) == UploadResult::Misaligned);
  CHECK(misaligned.accepted_uploads() == 0u);
}

TEST_CASE("upload: the simulator uploads the faces, icons and tiles through the contract: aligned, chunked, whole") {
  FakeDisplay fake;
  fake.record_uploads(kPlenty);
  CHECK(o3os::sim::load_assets(fake) == UploadResult::Ok);
  CHECK(fake.rejected_uploads() == 0u);
  // The faces go first, in order. OS-FLASHDIET A4: each is inflated from its deflated stream through the staging
  // chunk, so every upload carries the inflated bytes: the stream is word aligned (the generator declares it, ADR
  // 0011), the RAM_G address is word aligned, each chunk is at most the cap, and the bytes are the face's raw bitmap.
  std::size_t at = 0;
  for (unsigned i = 0; i < o3os::ui::kFontCount; ++i) {
    const o3os::ui::Font font = static_cast<o3os::ui::Font>(i);
    const o3os::ui::FontFace& face = o3os::ui::font_face(font);
    CHECK(reinterpret_cast<std::uintptr_t>(face.z) % kUploadAlign == 0);
    CHECK(face.ram_g_addr % kUploadAlign == 0);
    std::vector<std::uint8_t> px(face.bitmap_bytes);
    CHECK(o3os::ui::face_bitmap(font, px.data(), px.size()));
    std::size_t sent = 0;
    while (sent < face.bitmap_bytes && at < fake.uploads().size()) {
      const auto& u = fake.uploads()[at++];
      CHECK(u.bytes.size() <= kMaxUploadBytes);
      CHECK(u.addr == face.ram_g_addr + sent);
      CHECK(std::memcmp(u.bytes.data(), px.data() + sent, u.bytes.size()) == 0);
      sent += u.bytes.size();
    }
    CHECK(sent == face.bitmap_bytes);
  }
  // A display that refuses uploads leaves the assets incomplete and says so.
  FakeDisplay busy;
  busy.set_busy(true);
  CHECK(o3os::sim::load_assets(busy) == UploadResult::Busy);
  CHECK(busy.last_upload_result() == UploadResult::Busy);
  // On the simulator's display the faces land in RAM_G, where the renderer's BITMAP_SOURCE points.
  EveRaster raster;
  RasterDisplay display(raster);
  CHECK(o3os::sim::load_assets(display) == UploadResult::Ok);
  for (unsigned i = 0; i < o3os::ui::kFontCount; ++i) {
    const o3os::ui::Font font = static_cast<o3os::ui::Font>(i);
    const o3os::ui::FontFace& face = o3os::ui::font_face(font);
    std::vector<std::uint8_t> px(face.bitmap_bytes);
    CHECK(o3os::ui::face_bitmap(font, px.data(), px.size()));
    CHECK(std::memcmp(raster.ram_g() + face.ram_g_addr, px.data(), face.bitmap_bytes) == 0);
  }
}

TEST_CASE("upload: the simulator's display writes RAM_G at once and the next committed frame draws from it") {
  EveRaster raster;
  RasterDisplay display(raster);
  display.record_uploads(kPlenty);
  o3os::hal::Display& sink = display;
  Staging data{};
  data.bytes[0] = 0xF0;  // an 8 x 1 L1 bitmap: the left four pixels
  data.bytes[4] = 0x0F;  // and another at +4: the right four

  CHECK(sink.upload(64, data.bytes, 8) == UploadResult::Ok);
  CHECK(raster.ram_g()[64] == 0xF0);
  CHECK(raster.ram_g()[68] == 0x0F);
  CHECK(raster.ram_g()[63] == 0);  // nothing around it
  CHECK(raster.ram_g()[72] == 0);
  CHECK(display.uploads().size() == 1u);

  o3os::eve::DisplayListBuffer words;
  o3os::eve::DisplayListBuilder builder(display, words.words, o3os::eve::kMaxDisplayListWords);
  const auto frame = [&](std::uint32_t source) {
    CHECK(builder.begin_frame());
    builder.emit(dl::bitmap_handle(1));
    builder.emit(dl::bitmap_source(source));
    builder.emit(dl::bitmap_layout(BitmapFormat::L1, 1, 1));
    builder.emit(dl::bitmap_size(Filter::Nearest, Wrap::Border, Wrap::Border, 8, 1));
    builder.emit(dl::begin(Primitive::Bitmaps));
    builder.emit(dl::vertex2ii(10, 10, 1, 0));
    builder.emit(dl::end());
    CHECK(builder.end_frame());
  };
  const auto white_at = [&](int x) { return raster.rgba()[(10 * EveRaster::kWidth + x) * 4] == 255; };
  frame(64);
  for (int i = 0; i < 8; ++i) {
    CHECK(white_at(10 + i) == (i < 4));
  }
  // An upload made now, while this frame is the visible one, does not redraw it (unlike the chip); the next
  // committed frame shows it. Overwrite the first bitmap with the second's pattern.
  Staging other{};
  other.bytes[0] = 0x0F;
  CHECK(sink.upload(64, other.bytes, 4) == UploadResult::Ok);
  for (int i = 0; i < 8; ++i) {
    CHECK(white_at(10 + i) == (i < 4));  // still the old picture
  }
  frame(64);
  for (int i = 0; i < 8; ++i) {
    CHECK(white_at(10 + i) == (i >= 4));
  }
  // An upload inside an open frame is in RAM_G before that frame is committed, and is not undone by an abort.
  CHECK(builder.begin_frame());
  Staging third{};
  third.bytes[0] = 0xFF;
  CHECK(sink.upload(64, third.bytes, 4) == UploadResult::Ok);
  builder.emit(dl::bitmap_handle(1));
  builder.emit(dl::begin(Primitive::Bitmaps));
  builder.emit(dl::vertex2ii(10, 10, 1, 0));
  builder.emit(dl::end());
  CHECK(builder.end_frame());
  for (int i = 0; i < 8; ++i) {
    CHECK(white_at(10 + i));  // the bitmap registers persist, so the frame drew the new data
  }
  CHECK(raster.ram_g()[64] == 0xFF);
}

TEST_CASE("upload: the simulator's display refuses what the contract refuses and leaves RAM_G alone") {
  EveRaster raster;
  RasterDisplay display(raster);
  Staging data = make_staging();
  CHECK(display.upload(kRamGBytes - 4, data.bytes, 8) == UploadResult::BadRange);
  CHECK(display.upload(2, data.bytes, 4) == UploadResult::Misaligned);
  CHECK(display.upload(0, data.bytes + 1, 4) == UploadResult::Misaligned);
  CHECK(display.upload(0, data.bytes, kMaxUploadBytes + 4) == UploadResult::TooLarge);
  CHECK(display.last_upload_result() == UploadResult::TooLarge);
  CHECK(display.rejected_uploads() == 4u);
  CHECK(display.accepted_uploads() == 0u);
  for (std::size_t i = 0; i < 16; ++i) {
    CHECK(raster.ram_g()[i] == 0);
  }
  CHECK(display.upload(kRamGBytes - 8, data.bytes, 8) == UploadResult::Ok);
  CHECK(raster.ram_g()[kRamGBytes - 1] == data.bytes[7]);
  // The raster's own entry point has the same bounds.
  CHECK(raster.write_ram_g(0, data.bytes, 4));
  CHECK_FALSE(raster.write_ram_g(kRamGBytes - 1, data.bytes, 2));
  CHECK_FALSE(raster.write_ram_g(0xFFFFFFFFu, data.bytes, 2));
  CHECK_FALSE(raster.write_ram_g(0, nullptr, 1));
  CHECK(raster.write_ram_g(0, nullptr, 0));
}

TEST_CASE("alignment: display-list buffers are 8-byte aligned by type, and the builder refuses a misaligned one") {
  static_assert(alignof(o3os::eve::DisplayListBuffer) == 8, "explicit alignment");
  static_assert(sizeof(o3os::eve::DisplayListBuffer) == 4 * o3os::eve::kMaxDisplayListWords, "no padding");
  o3os::eve::DisplayListBuffer buffer;
  CHECK(reinterpret_cast<std::uintptr_t>(buffer.words) % 8 == 0);
  FakeDisplay sink;
  {
    o3os::eve::DisplayListBuilder builder(sink, buffer.words, o3os::eve::kMaxDisplayListWords);
    CHECK(builder.begin_frame());
    CHECK(builder.end_frame());
  }
  // A word pointer moved to an odd byte offset is never written through: no frame is begun.
  alignas(8) std::uint32_t raw[16] = {};
  for (std::size_t offset : {1u, 2u, 3u}) {
    auto* misaligned = reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(raw) + offset);
    o3os::eve::DisplayListBuilder builder(sink, misaligned, 8);
    const std::size_t begun = sink.begun();
    CHECK_FALSE(builder.begin_frame());
    CHECK(sink.begun() == begun);  // the sink was not even asked
    CHECK_FALSE(builder.emit(dl::end()));
    CHECK_FALSE(builder.end_frame());
  }
  for (const std::uint32_t word : raw) {
    CHECK(word == 0u);  // and nothing was stored
  }
  o3os::eve::DisplayListBuilder aligned(sink, raw, 8);
  CHECK(aligned.begin_frame());
  CHECK(aligned.emit(dl::end()));
  CHECK(aligned.end_frame());
}
