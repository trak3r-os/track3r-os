// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <o3os/eve/display_list_builder.h>
#include <o3os/eve/dl.h>
#include <o3os/platform/host/fakes.h>
#include <o3os/sim/eve_raster.h>
#include <o3os/sim/raster_display.h>
#include <o3os/ui/assets.h>
#include <o3os/ui/font.h>
#include <o3os/ui/renderer.h>
#include <o3os/ui/style.h>
#include <o3os/ui/theme.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace dl = o3os::eve::dl;
using namespace o3os;
using ui::Axis;
using ui::Color;
using ui::Face;
using ui::Rect;
using Words = std::vector<std::uint32_t>;

namespace {

const ui::Theme& amiga() { return ui::theme(ui::ThemeId::Amiga); }

// Runs `draw` between begin() and end() against a fake display; returns the committed list (DISPLAY last).
template <typename Fn>
Words frame_of(Fn draw, std::size_t capacity = eve::kMaxDisplayListWords, bool* complete = nullptr,
               const ui::Theme& theme = amiga()) {
  platform::host::FakeDisplay sink;
  std::vector<std::uint32_t> storage(capacity);
  eve::DisplayListBuilder builder(sink, storage.data(), capacity);
  ui::Renderer r(builder, theme);
  const bool began = r.begin(Color::Pane);
  CHECK(began);
  draw(r);
  const bool ok = r.end();
  if (complete != nullptr) {
    *complete = ok;
  }
  return sink.last_frame();
}

// What begin() emits: CLEAR_COLOR_RGB, CLEAR, LINE_WIDTH and the four words that bind each of the eight bitmap handles.
constexpr std::size_t kPreamble = 3 + 4 * (ui::kFontCount + ui::kTileSetCount);
static_assert(kPreamble == 35, "3 + 4 per handle, 8 handles");

Words body_of(const Words& frame) {  // the words between the preamble and DISPLAY
  return Words(frame.begin() + static_cast<std::ptrdiff_t>(kPreamble), frame.end() - 1);
}

std::uint32_t colour(Color c) {
  const ui::Rgb& v = amiga().rgb(c);
  return dl::color_rgb(v.r, v.g, v.b);
}
std::uint32_t rgb_of(Color c) {
  const ui::Rgb& v = amiga().rgb(c);
  return (std::uint32_t{v.r} << 16) | (std::uint32_t{v.g} << 8) | v.b;
}
std::uint32_t v(int x, int y) { return dl::vertex2f(x * 16, y * 16); }          // a bitmap at pixel (x, y)
std::uint32_t rc(int x, int y) { return dl::vertex2f(x * 16 + 8, y * 16 + 8); }  // centre of pixel (x, y): a rectangle corner
unsigned cell_of(char ch) { return static_cast<unsigned>(ch) - 32u; }

constexpr std::uint32_t kData = 2;     // the Data face's bitmap handle (10x20)
constexpr std::uint32_t kLabel = 3;    // the Label face (8x16)
constexpr std::uint32_t kMicro = 4;    // the Micro face (6x12)

// A frame drawn into the simulated panel with the assets uploaded to its RAM_G, as the device does at boot.
struct Panel {
  sim::EveRaster raster;
  sim::RasterDisplay display{raster};
  Panel() { CHECK(sim::load_assets(display) == hal::UploadResult::Ok); }
  template <typename Fn>
  bool draw(Fn fn, const ui::Theme& theme = amiga()) {
    std::vector<std::uint32_t> storage(eve::kMaxDisplayListWords);
    eve::DisplayListBuilder builder(display, storage.data(), eve::kMaxDisplayListWords);
    ui::Renderer r(builder, theme);
    CHECK(r.begin(Color::Pane));
    fn(r);
    return r.end() && display.render_failures() == 0;
  }
  std::uint32_t pixel(int x, int y) const {
    const std::uint8_t* p = raster.rgba() + (static_cast<std::size_t>(y) * 800 + static_cast<std::size_t>(x)) * 4;
    return (std::uint32_t{p[0]} << 16) | (std::uint32_t{p[1]} << 8) | p[2];
  }
  int count(std::uint32_t rgb) const {
    int n = 0;
    for (int y = 0; y < 480; ++y) {
      for (int x = 0; x < 800; ++x) {
        n += pixel(x, y) == rgb ? 1 : 0;
      }
    }
    return n;
  }
};

}  // namespace

TEST_CASE("ui: begin clears to the token's colour and binds the four faces, the tiles and the icons (hand-computed words)") {
  const Words f = frame_of([](ui::Renderer&) {});
  const Words expected = {
      0x020A0D14,  // CLEAR_COLOR_RGB(10, 13, 20) = Color::Pane of the Amiga theme
      0x26000007,  // CLEAR(colour, stencil, tag)
      0x0E000008,  // LINE_WIDTH(8): half a pixel; RECTS with vertices on pixel centres have exact edges
      0x05000001, 0x01000000, 0x07080420, 0x08002020,  // handle 1: Display 16x32 at 0, stride 2, 32 rows
      0x05000002, 0x010017C0, 0x07080414, 0x08001414,  // handle 2: Data 10x20 at 6080, stride 2, 20 rows
      0x05000003, 0x01002698, 0x07080210, 0x08001010,  // handle 3: Label 8x16 at 9880
      0x05000004, 0x01002C88, 0x0708020C, 0x08000C0C,  // handle 4: Micro 6x12 at 11400
      0x05000005, 0x01004000, 0x07080614, 0x08002814,  // handle 5: hex pairs 00..7F, 20x20 at 0x4000, stride 3
      0x05000006, 0x01005E00, 0x07080614, 0x08002814,  // handle 6: hex pairs 80..FF at +7680
      0x05000007, 0x01007C00, 0x07080814, 0x08003C14,  // handle 7: note tiles 0..127, 30x20, stride 4 (the other five have handle 12, below)
      0x05000008, 0x0100A590, 0x07080410, 0x08002010,  // handle 8: 11 icons, 16x16, stride 2, after 133 note tiles (0x7C00 + 133 * 80)
      0x00000000,  // DISPLAY
  };
  CHECK(f == expected);
  // The clear colour is whatever token begin() is given, in the theme the renderer was made with.
  const Words black = frame_of([](ui::Renderer&) {}, eve::kMaxDisplayListWords, nullptr, ui::theme(ui::ThemeId::Black));
  CHECK(black[0] == dl::clear_color_rgb(0, 0, 0));
  CHECK(black.size() == f.size());
}

TEST_CASE("ui: panels set the colour once per change and keep one BEGIN(RECTS)") {
  const Words f = frame_of([](ui::Renderer& r) {
    r.panel({10, 20, 30, 40}, Color::Chrome);
    r.panel({0, 0, 5, 5}, Color::Chrome);   // same colour: no second COLOR_RGB
    r.panel({1, 2, 3, 4}, Color::Sel);      // other colour: COLOR_RGB only
    r.panel({1, 1, 0, 9}, Color::Sel);      // empty: nothing at all
    r.panel({1, 1, 9, -2}, Color::Sel);
  });
  // A rectangle is its first and its last pixel, by their centres: {10,20,30,40} covers x 10..39, y 20..59.
  const Words expected = {colour(Color::Chrome), dl::begin(eve::Primitive::Rects), rc(10, 20), rc(39, 59), rc(0, 0),
                          rc(4, 4), colour(Color::Sel), rc(1, 2), rc(3, 5), dl::end(), dl::display()};
  CHECK(body_of(f) == Words(expected.begin(), expected.end() - 1));
  CHECK(f.back() == dl::display());
}

TEST_CASE("ui: frame is four 1 px rectangles inside the box, rules are one") {
  const Words f = frame_of([](ui::Renderer& r) {
    r.frame({10, 20, 30, 40}, Color::Empty);
    r.rule(5, 6, 7, Axis::Horizontal, Color::Empty);
    r.rule(5, 6, 7, Axis::Vertical, Color::ChromeDim);
    r.rule(0, 0, 0, Axis::Vertical, Color::Text);  // zero length: nothing
  });
  const Words expected = {colour(Color::Empty), dl::begin(eve::Primitive::Rects),
                          rc(10, 20), rc(39, 20),  // top
                          rc(10, 59), rc(39, 59),  // bottom
                          rc(10, 21), rc(10, 58),  // left
                          rc(39, 21), rc(39, 58),  // right
                          rc(5, 6),   rc(11, 6),   // horizontal rule
                          colour(Color::ChromeDim), rc(5, 6), rc(5, 12), dl::end()};
  CHECK(body_of(f) == expected);
}

TEST_CASE("ui: highlight_row is a Sel bar") {
  const Words a = frame_of([](ui::Renderer& r) { r.highlight_row({0, 100, 800, 24}); });
  const Words b = frame_of([](ui::Renderer& r) { r.panel({0, 100, 800, 24}, Color::Sel); });
  CHECK(a == b);
}

TEST_CASE("ui: text near the origin is one VERTEX2II per visible glyph, in the face the theme gives the role") {
  const Words f = frame_of([](ui::Renderer& r) { r.text(8, 16, "A B", Color::Text); });  // Data by default
  const Words expected = {colour(Color::Text), dl::begin(eve::Primitive::Bitmaps),
                          dl::vertex2ii(8, 16, kData, cell_of('A')),
                          dl::vertex2ii(28, 16, kData, cell_of('B')),  // the space still advances 10 px
                          dl::end()};
  CHECK(body_of(f) == expected);
  // A string with nothing visible changes nothing, not even the colour.
  const Words blank = frame_of([](ui::Renderer& r) { r.text(8, 16, "   ", Color::Text); r.text(0, 0, "", Color::Text); r.text(0, 0, nullptr, Color::Text); });
  CHECK(body_of(blank).empty());
  // The other roles use their own handles.
  const Words faces = frame_of([](ui::Renderer& r) {
    r.text(0, 0, "D", Color::Text, Face::Display);
    r.text(0, 40, "L", Color::Text, Face::Label);
    r.text(0, 80, "M", Color::Text, Face::Micro);
  });
  CHECK(body_of(faces) == Words{colour(Color::Text), dl::begin(eve::Primitive::Bitmaps),
                                dl::vertex2ii(0, 0, 1, cell_of('D')), dl::vertex2ii(0, 40, kLabel, cell_of('L')),
                                dl::vertex2ii(0, 80, kMicro, cell_of('M')), dl::end()});
}

TEST_CASE("ui: characters outside the face are drawn as '?'") {
  const Words f = frame_of([](ui::Renderer& r) { r.text(0, 0, "\x01~\x7f", Color::Text); });
  const Words expected = {colour(Color::Text), dl::begin(eve::Primitive::Bitmaps),
                          dl::vertex2ii(0, 0, kData, cell_of('?')), dl::vertex2ii(10, 0, kData, cell_of('~')),
                          dl::vertex2ii(20, 0, kData, cell_of('?')), dl::end()};
  CHECK(body_of(f) == expected);
}

TEST_CASE("ui: past x = 511 the origin moves (VERTEX_TRANSLATE_X) and the glyphs stay one VERTEX2II word each") {
  // x = 600: origin 600 - 256 = 344, down to a multiple of 64 = 320 (5120 sixteenths); 600 is then 280 into the window.
  const Words f = frame_of([](ui::Renderer& r) { r.text(600, 40, "AAB", Color::Text); });
  const Words expected = {colour(Color::Text), dl::begin(eve::Primitive::Bitmaps), dl::vertex_translate_x(320 * 16),
                          dl::vertex2ii(280, 40, kData, cell_of('A')), dl::vertex2ii(290, 40, kData, cell_of('A')),
                          dl::vertex2ii(300, 40, kData, cell_of('B')), dl::end()};
  CHECK(body_of(f) == expected);

  // The origin stays while the next bitmaps are reachable (both ways), and returns for x < the window.
  const Words g = frame_of([](ui::Renderer& r) {
    r.text(600, 40, "A", Color::Text);
    r.text(400, 40, "A", Color::Text);  // 80 into the window: reachable, no new translate
    r.text(10, 40, "A", Color::Text);   // left of it: the origin moves back to 0
  });
  const Words expected_g = {colour(Color::Text), dl::begin(eve::Primitive::Bitmaps), dl::vertex_translate_x(320 * 16),
                            dl::vertex2ii(280, 40, kData, cell_of('A')), dl::vertex2ii(80, 40, kData, cell_of('A')),
                            dl::vertex_translate_x(0), dl::vertex2ii(10, 40, kData, cell_of('A')), dl::end()};
  CHECK(body_of(g) == expected_g);

  // Rectangles follow the origin too, so a panel drawn after a translated glyph is still where it was asked for.
  const Words h = frame_of([](ui::Renderer& r) {
    r.text(600, 40, "A", Color::Text);
    r.panel({700, 0, 10, 10}, Color::Sel);
  });
  CHECK(body_of(h).back() == dl::end());
  CHECK(h[h.size() - 3] == rc(709 - 320, 9));  // the second corner, 389 into the window
}

TEST_CASE("ui: without translation the glyphs past x = 511 use BITMAP_HANDLE + CELL + VERTEX2F and share what they can") {
  const auto draw = [](ui::Renderer& r) { r.text(600, 40, "AAB", Color::Text); };
  platform::host::FakeDisplay sink;
  std::vector<std::uint32_t> storage(eve::kMaxDisplayListWords);
  eve::DisplayListBuilder builder(sink, storage.data(), eve::kMaxDisplayListWords);
  ui::Renderer r(builder, amiga());
  r.use_translate(false);
  CHECK(r.begin(Color::Pane));
  draw(r);
  r.text(10, 10, "A", Color::Text);
  r.text(600, 60, "A", Color::Text);
  r.text(600, 80, "A", Color::Text, Face::Label);
  r.text(10, 512, "A", Color::Text);  // y past 511: VERTEX2F whatever the origin is
  CHECK(r.end());
  const Words f = sink.last_frame();
  const Words expected = {colour(Color::Text), dl::begin(eve::Primitive::Bitmaps),
                          dl::bitmap_handle(kData), dl::cell(cell_of('A')), v(600, 40),
                          v(610, 40),                                    // same cell: just the vertex
                          dl::cell(cell_of('B')), v(620, 40),
                          dl::vertex2ii(10, 10, kData, cell_of('A')),  // after VERTEX2II the registers are not trusted
                          dl::bitmap_handle(kData), dl::cell(cell_of('A')), v(600, 60),
                          dl::bitmap_handle(kLabel), dl::cell(cell_of('A')), v(600, 80),
                          dl::bitmap_handle(kData), dl::cell(cell_of('A')), v(10, 512), dl::end()};
  CHECK(body_of(f) == expected);
  for (const std::uint32_t w : f) {
    CHECK((w >> 24) != 0x2B);  // no VERTEX_TRANSLATE_X anywhere
  }
}

TEST_CASE("ui: text_grid places text on the face's character grid, text_width is cells") {
  const Words a = frame_of([](ui::Renderer& r) { r.text_grid(100, 200, 3, 2, "X", Color::Text); });
  const Words b = frame_of([](ui::Renderer& r) { r.text(100 + 3 * 10, 200 + 2 * 20, "X", Color::Text); });
  CHECK(a == b);
  const Words c = frame_of([](ui::Renderer& r) { r.text_grid(100, 200, 3, 2, "X", Color::Text, Face::Label); });
  const Words d = frame_of([](ui::Renderer& r) { r.text(100 + 3 * 8, 200 + 2 * 16, "X", Color::Text, Face::Label); });
  CHECK(c == d);
  const Words e = frame_of([](ui::Renderer& r) {
    CHECK(r.text_width("PATTERN", Face::Label) == 56);
    CHECK(r.text_width("C-4 01", Face::Data) == 60);  // spaces advance too
    CHECK(r.text_width("128.0", Face::Display) == 80);
    CHECK(r.text_width("x", Face::Micro) == 6);
    CHECK(r.text_width("", Face::Data) == 0);
    CHECK(r.text_width(nullptr, Face::Data) == 0);
  });
  CHECK(e.size() == kPreamble + 1);
}

TEST_CASE("ui: tiles and icons are one VERTEX2II word each, on the handle that holds them") {
  const Words f = frame_of([](ui::Renderer& r) {
    r.hex_tile(10, 20, 0x00, Color::Text);
    r.hex_tile(40, 20, 0x7F, Color::Text);   // the last of the first handle
    r.hex_tile(70, 20, 0x80, Color::Text);   // the first of the second
    r.hex_tile(100, 20, 0x1FF, Color::Text);  // masked to 0xFF
    r.note_tile(10, 50, 0, Color::Empty);
    r.note_tile(50, 50, 120, Color::Empty);
    r.note_tile(90, 50, 500, Color::Empty);  // beyond the set: "???"
    r.icon(10, 80, ui::Icon::Warning, Color::Accent);
  });
  const Words expected = {colour(Color::Text), dl::begin(eve::Primitive::Bitmaps),
                          dl::vertex2ii(10, 20, 5, 0), dl::vertex2ii(40, 20, 5, 127), dl::vertex2ii(70, 20, 6, 0),
                          dl::vertex2ii(100, 20, 6, 127),
                          colour(Color::Empty), dl::vertex2ii(10, 50, 7, 0), dl::vertex2ii(50, 50, 7, 120),
                          dl::vertex2ii(90, 50, 7, 122),
                          colour(Color::Accent), dl::vertex2ii(10, 80, 8, 10), dl::end()};
  CHECK(body_of(f) == expected);
  // An icon that is not one draws nothing.
  const Words none = frame_of([](ui::Renderer& r) { r.icon(0, 0, static_cast<ui::Icon>(ui::kIconCount), Color::Text); });
  CHECK(body_of(none).empty());
}

TEST_CASE("ui: the note tiles past the 128th are drawn with a second handle that the first of them binds, once, and no other frame pays for") {
  // The bind: BITMAP_HANDLE 12, the source 128 tiles after the first handle's (0x7C00 + 128 * 80), the same layout and size.
  const Words bind = {dl::bitmap_handle(12), dl::bitmap_source(0xA400u), dl::bitmap_layout(eve::BitmapFormat::L1, 4, 20),
                      dl::bitmap_size(eve::Filter::Nearest, eve::Wrap::Border, eve::Wrap::Border, 30, 20)};
  CHECK(bind[0] == 0x0500000C);
  CHECK(bind[1] == 0x0100A400);
  CHECK(ui::notes_hi_set().ram_g == 0xA400u);
  const Words f = frame_of([](ui::Renderer& r) {
    r.note_tile(10, 50, 5, Color::Empty);                  // the first handle: nothing to bind
    r.note_tile(50, 50, 128, Color::Empty);                // the first cell of the second
    r.note_tile(90, 50, ui::kNoteTiles - 1u, Color::Empty);  // its last: already bound
    r.note_tile(130, 50, 127, Color::Empty);               // the last of the first
    r.note_tile(170, 50, ui::kNoteTiles + 7u, Color::Empty);  // beyond the set: "???", in the first handle
  });
  Words expected = {colour(Color::Empty), dl::begin(eve::Primitive::Bitmaps), dl::vertex2ii(10, 50, 7, 5)};
  expected.insert(expected.end(), bind.begin(), bind.end());
  expected.push_back(dl::vertex2ii(50, 50, 12, 0));
  expected.push_back(dl::vertex2ii(90, 50, 12, 4));
  expected.push_back(dl::vertex2ii(130, 50, 7, 127));
  expected.push_back(dl::vertex2ii(170, 50, 7, 122));
  expected.push_back(dl::end());
  CHECK(body_of(f) == expected);
  // A frame of the first handle's tiles alone has no bind word (the preamble is the same eight handles as before), and the next frame binds again.
  const Words plain = frame_of([](ui::Renderer& r) { r.note_tile(10, 50, 127, Color::Text); });
  for (const std::uint32_t w : plain) {
    CHECK(w != 0x0500000C);
  }
  CHECK(plain.size() == kPreamble + 5);  // the preamble, colour, BEGIN, the vertex, END, DISPLAY
  const Words twice = frame_of([](ui::Renderer& r) { r.note_tile(10, 50, 130, Color::Text); });
  CHECK(twice.size() == kPreamble + 5 + 4);  // and the four words of the bind
}

TEST_CASE("ui: without translation a tile of the second note handle past x = 511 follows the bind with CELL + VERTEX2F and switches handles by BITMAP_HANDLE") {
  platform::host::FakeDisplay sink;
  std::vector<std::uint32_t> storage(eve::kMaxDisplayListWords);
  eve::DisplayListBuilder builder(sink, storage.data(), eve::kMaxDisplayListWords);
  ui::Renderer r(builder, amiga());
  r.use_translate(false);
  CHECK(r.begin(Color::Pane));
  r.note_tile(600, 40, 130, Color::Text);  // binds; the bind leaves handle 12 current, so no second BITMAP_HANDLE
  r.note_tile(640, 40, 131, Color::Text);
  r.note_tile(700, 40, 5, Color::Text);    // the first handle
  r.note_tile(740, 40, 131, Color::Text);  // back: bound already, so only the switch
  CHECK(r.end());
  const Words f = sink.last_frame();
  const Words expected = {colour(Color::Text),
                          dl::bitmap_handle(12), dl::bitmap_source(0xA400u), dl::bitmap_layout(eve::BitmapFormat::L1, 4, 20),
                          dl::bitmap_size(eve::Filter::Nearest, eve::Wrap::Border, eve::Wrap::Border, 30, 20),
                          dl::begin(eve::Primitive::Bitmaps), dl::cell(2), v(600, 40),
                          dl::cell(3), v(640, 40),
                          dl::bitmap_handle(7), dl::cell(5), v(700, 40),
                          dl::bitmap_handle(12), dl::cell(3), v(740, 40), dl::end()};
  CHECK(body_of(f) == expected);
}

TEST_CASE("ui: switching between rectangles and bitmaps closes the primitive") {
  const Words f = frame_of([](ui::Renderer& r) {
    r.panel({0, 0, 8, 8}, Color::Chrome);
    r.text(0, 0, "A", Color::Text);
    r.panel({0, 0, 8, 8}, Color::Sel);
  });
  // The colour is state, so it is set first; END / BEGIN follow when the primitive changes.
  const Words expected = {colour(Color::Chrome), dl::begin(eve::Primitive::Rects), rc(0, 0), rc(7, 7),
                          colour(Color::Text), dl::end(), dl::begin(eve::Primitive::Bitmaps),
                          dl::vertex2ii(0, 0, kData, cell_of('A')),
                          colour(Color::Sel), dl::end(), dl::begin(eve::Primitive::Rects), rc(0, 0), rc(7, 7),
                          dl::end()};
  CHECK(body_of(f) == expected);
}

TEST_CASE("ui: a frame that overflows the list is reported, stops early and is never committed") {
  platform::host::FakeDisplay sink;
  std::uint32_t storage[64];
  eve::DisplayListBuilder builder(sink, storage, 64);
  ui::Renderer r(builder, amiga());
  // The preamble is 35 words: a 64-word list takes 28 more.
  CHECK(r.begin(Color::Pane));  // a small complete frame first: it must stay on screen
  r.text(0, 0, "ok", Color::Text);
  CHECK(r.end());
  const Words shown = sink.last_frame();
  CHECK(sink.frames() == 1u);

  CHECK(r.begin(Color::Pane));
  for (int i = 0; i < 200; ++i) {
    r.text(0, 0, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", Color::Text);
  }
  CHECK_FALSE(r.ok());
  CHECK(r.words() == 63u);  // stopped at capacity - 1: the loop does not keep emitting into a full list
  CHECK_FALSE(r.end());
  CHECK(sink.frames() == 1u);
  CHECK(sink.aborted() == 1u);
  CHECK(sink.last_frame() == shown);  // the previous frame is still the visible one
}

TEST_CASE("ui font: geometry, slots, alignment and glyph data of the four faces") {
  struct Want {
    ui::Font font;
    unsigned w, h, stride, ascent, cap;
  };
  const Want want[] = {{ui::Font::F16x32, 16, 32, 2, 26, 20},
                       {ui::Font::F10x20, 10, 20, 2, 16, 13},
                       {ui::Font::F8x16, 8, 16, 1, 12, 10},
                       {ui::Font::F6x12, 6, 12, 1, 9, 8}};
  std::uint32_t end = 0;
  for (unsigned i = 0; i < ui::kFontCount; ++i) {
    const ui::FontFace& f = ui::font_face(want[i].font);
    CHECK((f.cell_w == want[i].w && f.cell_h == want[i].h && f.stride == want[i].stride));
    CHECK((f.ascent == want[i].ascent && f.cap_h == want[i].cap));
    CHECK(f.first_char == 32);
    CHECK(f.glyph_count == 95);
    CHECK(f.bitmap_bytes == f.glyph_count * f.stride * f.cell_h);
    CHECK(f.handle == i + 1);                                        // handles 1..4, in ui::Font order
    CHECK(f.ram_g_addr % hal::kUploadAlign == 0);
    CHECK(f.ram_g_addr >= end);                                      // packed in order, no overlap
    end = f.ram_g_addr + f.bitmap_bytes;
    CHECK(reinterpret_cast<std::uintptr_t>(f.z) % hal::kUploadAlign == 0);  // the stream is declared alignas(4) by the generator
    // OS-FLASHDIET A4: the face is stored deflated; inflate it and check the glyph data of the raw bitmap.
    std::vector<std::uint8_t> px(f.bitmap_bytes);
    CHECK(ui::face_bitmap(want[i].font, px.data(), px.size()));
    // The compile-time geometry layouts use is the geometry of the generated bitmaps.
    const ui::FontGeometry g = ui::font_geometry(want[i].font);
    CHECK((g.cell_w == f.cell_w && g.cell_h == f.cell_h && g.ascent == f.ascent && g.cap_h == f.cap_h));
    // Space is blank, every other printable character has ink, and nothing lies right of the cell's width.
    for (unsigned c = 0; c < f.glyph_count; ++c) {
      unsigned ink = 0;
      for (unsigned row = 0; row < f.cell_h; ++row) {
        for (unsigned b = 0; b < f.stride; ++b) {
          const std::uint8_t bits = px[c * f.stride * f.cell_h + row * f.stride + b];
          ink += bits;
          if (b + 1 == f.stride) {
            CHECK((bits & (0xFFu >> (f.cell_w - 8 * (f.stride - 1)))) == 0);  // the pad bits of the last byte
          }
        }
      }
      CHECK((ink == 0) == (c == 0));
    }
  }
  CHECK(end <= ui::kRamGTiles);  // the faces end below where the tiles start
  // 'A' of Spleen 8x16, as in the BDF: rows 2..11 are 7C C6 C6 C6 FE C6 C6 C6 C6 C6.
  const std::uint8_t a[16] = {0, 0, 0x7C, 0xC6, 0xC6, 0xC6, 0xFE, 0xC6, 0xC6, 0xC6, 0xC6, 0xC6, 0, 0, 0, 0};
  std::vector<std::uint8_t> px8(ui::font_face(ui::Font::F8x16).bitmap_bytes);
  CHECK(ui::face_bitmap(ui::Font::F8x16, px8.data(), px8.size()));
  CHECK(std::memcmp(px8.data() + cell_of('A') * 16, a, 16) == 0);
  // 'A' of Spleen 6x12: 70 88 88 88 F8 88 88 88 on rows 1..8.
  const std::uint8_t sa[12] = {0, 0x70, 0x88, 0x88, 0x88, 0xF8, 0x88, 0x88, 0x88, 0, 0, 0};
  std::vector<std::uint8_t> px6(ui::font_face(ui::Font::F6x12).bitmap_bytes);
  CHECK(ui::face_bitmap(ui::Font::F6x12, px6.data(), px6.size()));
  CHECK(std::memcmp(px6.data() + cell_of('A') * 12, sa, 12) == 0);
  // 'C' of Terminus Bold 10x20 (ter-u20b.bdf): ..######.. on row 3, .##....##. on rows 4 and 5.
  std::vector<std::uint8_t> px20(ui::font_face(ui::Font::F10x20).bitmap_bytes);
  CHECK(ui::face_bitmap(ui::Font::F10x20, px20.data(), px20.size()));
  const std::uint8_t* c20 = px20.data() + cell_of('C') * 40;
  CHECK((c20[3 * 2] == 0x3F && c20[3 * 2 + 1] == 0x00));
  CHECK((c20[4 * 2] == 0x61 && c20[4 * 2 + 1] == 0x80));
  CHECK(std::memcmp(c20 + 4 * 2, c20 + 5 * 2, 2) == 0);
}

TEST_CASE("ui: text drawn through the display list reaches the framebuffer pixel for pixel, in every face") {
  auto panel = std::make_unique<Panel>();
  CHECK(panel->draw([](ui::Renderer& r) {
    r.panel({0, 0, 800, 480}, Color::Chrome);
    r.text(20, 30, "Hi~", Color::Text);                   // Data, near
    r.text(620, 330, "Hi~", Color::Text, Face::Label);    // Label, far: past the 9-bit window
    r.text(300, 100, "Hi~", Color::Text, Face::Display);
    r.text(700, 200, "Hi~", Color::Text, Face::Micro);
  }));
  const auto check_text = [&](const char* s, int x0, int y0, ui::Font font) {
    const ui::FontFace& f = ui::font_face(font);
    std::vector<std::uint8_t> px(f.bitmap_bytes);  // OS-FLASHDIET A4: inflate the face for its raw pixels
    CHECK(ui::face_bitmap(font, px.data(), px.size()));
    int mismatches = 0;
    for (int i = 0; s[i] != '\0'; ++i) {
      const unsigned cell = cell_of(s[i]);
      for (int row = 0; row < f.cell_h; ++row) {
        for (int col = 0; col < f.cell_w; ++col) {
          const std::uint8_t bits =
              px[cell * f.stride * f.cell_h + static_cast<unsigned>(row) * f.stride + static_cast<unsigned>(col / 8)];
          const bool ink = (bits >> (7 - col % 8)) & 1;
          const std::uint32_t want = ink ? rgb_of(Color::Text) : rgb_of(Color::Chrome);
          mismatches += panel->pixel(x0 + i * f.cell_w + col, y0 + row) != want ? 1 : 0;
        }
      }
    }
    return mismatches;
  };
  CHECK(check_text("Hi~", 20, 30, ui::Font::F10x20) == 0);
  CHECK(check_text("Hi~", 620, 330, ui::Font::F8x16) == 0);
  CHECK(check_text("Hi~", 300, 100, ui::Font::F16x32) == 0);
  CHECK(check_text("Hi~", 700, 200, ui::Font::F6x12) == 0);
}

TEST_CASE("ui: panels, frames and rules cover exactly the pixels asked for, edges on pixel boundaries") {
  auto panel = std::make_unique<Panel>();
  CHECK(panel->draw([](ui::Renderer& r) {
    r.panel({10, 20, 30, 40}, Color::Chrome);
    r.frame({100, 100, 10, 6}, Color::Empty);
    r.rule(50, 60, 7, Axis::Horizontal, Color::Accent);
    r.rule(70, 60, 5, Axis::Vertical, Color::Accent);
    r.panel({200, 200, 1, 1}, Color::Text);  // a single pixel
    r.panel({700, 300, 20, 5}, Color::Play);  // right of the 9-bit window after a translated glyph
    r.text(650, 400, "A", Color::Text);
    r.panel({600, 300, 20, 5}, Color::Vol);
  }));
  const auto pixel = [&](int x, int y) { return panel->pixel(x, y); };
  CHECK(panel->count(rgb_of(Color::Chrome)) == 30 * 40);
  CHECK((pixel(10, 20) == rgb_of(Color::Chrome) && pixel(39, 59) == rgb_of(Color::Chrome)));
  CHECK((pixel(9, 20) != rgb_of(Color::Chrome) && pixel(40, 59) != rgb_of(Color::Chrome) &&
         pixel(10, 19) != rgb_of(Color::Chrome) && pixel(39, 60) != rgb_of(Color::Chrome)));
  CHECK(panel->count(rgb_of(Color::Empty)) == 2 * 10 + 2 * 4);  // top and bottom 10 px, left and right 4 px between them
  CHECK((pixel(100, 100) == rgb_of(Color::Empty) && pixel(109, 105) == rgb_of(Color::Empty)));
  CHECK((pixel(101, 101) != rgb_of(Color::Empty) && pixel(108, 104) != rgb_of(Color::Empty)));  // inside: not filled
  CHECK(panel->count(rgb_of(Color::Accent)) == 7 + 5);
  CHECK((pixel(50, 60) == rgb_of(Color::Accent) && pixel(56, 60) == rgb_of(Color::Accent) &&
         pixel(57, 60) != rgb_of(Color::Accent)));
  CHECK((pixel(70, 60) == rgb_of(Color::Accent) && pixel(70, 64) == rgb_of(Color::Accent) &&
         pixel(70, 65) != rgb_of(Color::Accent)));
  CHECK(pixel(200, 200) == rgb_of(Color::Text));
  CHECK(panel->count(rgb_of(Color::Play)) == 20 * 5);  // a rectangle is where it was asked for, wherever the origin is
  CHECK(panel->count(rgb_of(Color::Vol)) == 20 * 5);
  CHECK((pixel(700, 300) == rgb_of(Color::Play) && pixel(719, 304) == rgb_of(Color::Play)));
  CHECK((pixel(600, 300) == rgb_of(Color::Vol) && pixel(619, 304) == rgb_of(Color::Vol)));
}
