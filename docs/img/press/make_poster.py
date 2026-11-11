#!/usr/bin/env python3
# Promo poster for track3r-os v1.3.0 in the style of a 90s Amiga magazine ad.
# Everything is drawn pixel by pixel: the tracker's own bitmap fonts (Terminus, Spleen), its Amiga
# theme palette, real simulator screenshots at 1:1, copper bars, ordered dithering. No smoothing.
import sys, os
from PIL import Image, ImageDraw

ROOT = sys.argv[1]
SHOTS = sys.argv[2:6]
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'track3r-os-v1.3.0-poster.png')

# The firmware's Amiga theme (ui/src/themes.cpp) plus a few 32-colour-era extras.
CHROME = (178, 186, 202); CHROME_TXT = (14, 18, 28); CHROME_DIM = (60, 70, 96)
PANE = (10, 13, 20); TEXT = (230, 234, 242); DIM = (128, 136, 156)
SEL = (28, 92, 208); ORANGE = (255, 138, 0); GREEN = (84, 200, 92); RED = (190, 30, 30)
WB_BLUE = (0, 85, 170); WHITE = (255, 255, 255); BLACK = (0, 0, 0)
YELLOW = (255, 221, 0); CYAN = (126, 196, 240); PURPLE = (212, 166, 238)


def load_bdf(path):
    glyphs, w, h, desc = {}, 0, 0, 0
    with open(path, encoding='latin-1') as f:
        lines = f.read().split('\n')
    i = 0
    while i < len(lines):
        l = lines[i]
        if l.startswith('FONTBOUNDINGBOX'):
            _, w, h, _, desc = l.split(); w, h, desc = int(w), int(h), int(desc)
        if l.startswith('ENCODING'):
            code = int(l.split()[1])
        if l.startswith('BBX'):
            _, bw, bh, bx, by = l.split(); bw, bh, bx, by = int(bw), int(bh), int(bx), int(by)
        if l == 'BITMAP':
            rows = []
            i += 1
            while lines[i] != 'ENDCHAR':
                rows.append(int(lines[i], 16)); i += 1
            nbits = ((bw + 7) // 8) * 8
            cell = [[0] * w for _ in range(h)]
            top = h + desc - by - bh
            for r, v in enumerate(rows):
                for c in range(bw):
                    if v >> (nbits - 1 - c) & 1:
                        y, x = top + r, bx + c
                        if 0 <= y < h and 0 <= x < w:
                            cell[y][x] = 1
            glyphs[code] = cell
        i += 1
    return glyphs, w, h


F = {
    32: load_bdf(f'{ROOT}/third_party/fonts/terminus/ter-u32b.bdf'),
    20: load_bdf(f'{ROOT}/third_party/fonts/terminus/ter-u20b.bdf'),
    16: load_bdf(f'{ROOT}/third_party/fonts/spleen/spleen-8x16.bdf'),
    12: load_bdf(f'{ROOT}/third_party/fonts/spleen/spleen-6x12.bdf'),
}


def text(img, x, y, s, face=16, scale=1, color=TEXT, shadow=None, fill=None):
    """Blit bitmap text. fill(row, height) -> colour gives raster-bar letter fills."""
    g, w, h = F[face]
    px = img.load()
    for ch in s:
        cell = g.get(ord(ch), g.get(32))
        for r in range(h):
            for c in range(w):
                if cell[r][c]:
                    col = fill(r, h) if fill else color
                    for dy in range(scale):
                        for dx in range(scale):
                            if shadow:
                                sx, sy = x + c * scale + dx + shadow[0], y + r * scale + dy + shadow[1]
                                if 0 <= sx < img.width and 0 <= sy < img.height and px[sx, sy] != col:
                                    pass
            # draw after shadows so shadows never cover glyphs
        if shadow:
            for r in range(h):
                for c in range(w):
                    if cell[r][c]:
                        x0, y0 = x + c * scale + shadow[0], y + r * scale + shadow[1]
                        img.paste(shadow[2], (x0, y0, x0 + scale, y0 + scale))
        x += w * scale
    return x


def text2(img, x, y, s, face=16, scale=1, color=TEXT, shadow=None, fill=None):
    g, w, h = F[face]
    if shadow:
        text_plain(img, x + shadow[0], y + shadow[1], s, g, w, h, scale, lambda r, hh: shadow[2])
    text_plain(img, x, y, s, g, w, h, scale, fill or (lambda r, hh: color))
    return x + len(s) * w * scale


def text_plain(img, x, y, s, g, w, h, scale, fill):
    for ch in s:
        cell = g.get(ord(ch), g.get(32))
        for r in range(h):
            col = fill(r, h)
            for c in range(w):
                if cell[r][c]:
                    x0, y0 = x + c * scale, y + r * scale
                    img.paste(col, (x0, y0, x0 + scale, y0 + scale))
        x += w * scale


def width(s, face, scale=1):
    return len(s) * F[face][1] * scale


BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]


def dither_band(img, box, c0, c1, horizontal=False):
    """Ordered-dither a two-colour ramp, the way 32-colour screens faked gradients."""
    x0, y0, x1, y1 = box
    px = img.load()
    span = (x1 - x0) if horizontal else (y1 - y0)
    for y in range(y0, y1):
        for x in range(x0, x1):
            t = ((x - x0) if horizontal else (y - y0)) / max(1, span - 1)
            px[x, y] = c1 if t * 16 > BAYER[(y // 2) % 4][(x // 2) % 4] + 0.5 else c0


def copper(img, y, height, hues):
    """A copper bar: symmetric steps of one hue, 2-pixel lines like a lowres screen doubled."""
    d = ImageDraw.Draw(img)
    n = len(hues)
    steps = hues + hues[::-1]
    lh = max(2, height // len(steps))
    for i, c in enumerate(steps):
        d.rectangle((0, y + i * lh, img.width - 1, y + (i + 1) * lh - 1), fill=c)
    return y + len(steps) * lh


def ramp(c0, c1, n):
    return [tuple(int(c0[k] + (c1[k] - c0[k]) * i / (n - 1)) for k in range(3)) for i in range(n)]


def wb_window(img, box, title, active=True):
    """A Workbench-style window: title bar with drag stripes, close and depth gadgets, 2 px frame."""
    d = ImageDraw.Draw(img)
    x0, y0, x1, y1 = box
    d.rectangle(box, fill=CHROME)
    d.rectangle((x0, y0, x1, y0 + 25), fill=WB_BLUE if active else CHROME_DIM)
    for yy in range(y0 + 5, y0 + 22, 4):  # drag stripes
        d.rectangle((x0 + 36, yy, x1 - 70, yy + 1), fill=(0, 60, 130))
    tw = width(' ' + title + ' ', 16)
    d.rectangle((x0 + 40, y0 + 3, x0 + 40 + tw, y0 + 22), fill=WB_BLUE)
    text2(img, x0 + 40, y0 + 5, ' ' + title + ' ', 16, 1, WHITE)
    # close gadget
    d.rectangle((x0 + 4, y0 + 4, x0 + 26, y0 + 21), fill=CHROME)
    d.rectangle((x0 + 11, y0 + 9, x0 + 19, y0 + 16), fill=BLACK)
    d.rectangle((x0 + 13, y0 + 11, x0 + 17, y0 + 14), fill=WHITE)
    # depth gadgets
    for k, gx in enumerate((x1 - 60, x1 - 30)):
        d.rectangle((gx, y0 + 4, gx + 26, y0 + 21), fill=CHROME)
        d.rectangle((gx + 4, y0 + 7, gx + 15, y0 + 15), fill=BLACK if k == 0 else WHITE, outline=BLACK)
        d.rectangle((gx + 10, y0 + 11, gx + 21, y0 + 19), fill=WHITE if k == 0 else BLACK, outline=BLACK)
    # bevel
    d.line((x0, y0, x1, y0), fill=WHITE); d.line((x0, y0, x0, y1), fill=WHITE)
    d.line((x1, y0, x1, y1), fill=BLACK); d.line((x0, y1, x1, y1), fill=BLACK)
    return (x0 + 6, y0 + 32, x1 - 6, y1 - 6)


def starburst(img, cx, cy, r0, r1, n, color, edge):
    import math
    pts = []
    for i in range(n * 2):
        a = math.pi * i / n - math.pi / 2
        r = r1 if i % 2 == 0 else r0
        pts.append((round(cx + r * math.cos(a)), round(cy + r * math.sin(a))))
    d = ImageDraw.Draw(img)
    # chunky: draw at half size then scale would blur; instead draw outline twice for a hard 3 px edge
    d.polygon(pts, fill=edge)
    inner = [(round(cx + (x - cx) * 0.92), round(cy + (y - cy) * 0.92)) for x, y in pts]
    d.polygon(inner, fill=color)


def checker_floor(img, y0, y1, c0, c1):
    """Perspective checkerboard to the horizon, hard pixels, no anti-aliasing."""
    px = img.load()
    W = img.width
    horizon = y0
    for y in range(y0, y1):
        z = (y - horizon + 1)
        depth = 4000 / z
        for x in range(W):
            u = (x - W / 2) * depth / 600
            v = depth
            px[x, y] = c0 if (int(u // 40) + int(v // 40)) % 2 == 0 else c1


def grid_floor(img, y0, y1):
    """90s vector floor: black ground, blue lines converging on a vanishing point, rows closing up to the horizon."""
    d = ImageDraw.Draw(img)
    W = img.width
    d.rectangle((0, y0, W, y1), fill=BLACK)
    d.line((0, y0, W, y0), fill=ORANGE, width=2)
    vx, vy = W // 2, y0 - 120
    for k in range(-24, 25):
        xb = W // 2 + k * 120
        xt = vx + (xb - vx) * (y0 - vy) / (y1 - vy)
        d.line((round(xt), y0, xb, y1), fill=SEL)
    z = 1.0
    while True:
        y = vy + (y1 - vy) / z
        if y < y0 + 3:
            break
        d.line((0, round(y), W, round(y)), fill=SEL)
        z *= 1.35


def main():
    W, H = 1800, 2250
    img = Image.new('RGB', (W, H), PANE)
    d = ImageDraw.Draw(img)

    # --- header: copper bars behind a raster-filled logo
    y = 0
    dither_band(img, (0, 0, W, 300), (0, 0, 40), (20, 30, 90))
    copper(img, 26, 36, ramp((40, 0, 60), (255, 120, 255), 9))
    copper(img, 262, 36, ramp((0, 30, 60), (120, 220, 255), 9))

    logo = 'TRACK3R-OS'
    sc = 7
    lw = width(logo, 32, sc)
    lx = (W - lw) // 2
    chrome_rows = ramp((255, 255, 255), (110, 170, 255), 15) + [(20, 20, 60)] + ramp((255, 120, 0), (255, 240, 120), 16)

    def logo_fill(r, h):
        return chrome_rows[min(len(chrome_rows) - 1, r * len(chrome_rows) // h)]
    text2(img, lx, 40, logo, 32, sc, shadow=(10, 10, BLACK), fill=logo_fill)

    # version tag on a slanted-looking orange box (pixel steps)
    vt = 'V1.3.0'
    vw = width(vt, 32, 3)
    vx, vy = W - vw - 90, 250
    for k in range(6):
        d.rectangle((vx - 20 + k * 3, vy - 8 + k * 0, vx + vw + 20 + k * 3, vy + 104), fill=ORANGE if k == 5 else (120, 60, 0))
    text2(img, vx + 15, vy + 4, vt, 32, 3, BLACK)

    sub = 'FREE COMMUNITY FIRMWARE FOR THE POLYEND TRACKER'
    text2(img, 70, 322, sub, 20, 2, YELLOW, shadow=(3, 3, BLACK))

    # --- starburst
    bx, by = 1555, 478
    starburst(img, bx, by, 98, 140, 16, YELLOW, RED)
    text2(img, bx - width('FREE!', 32, 2) // 2, by - 50, 'FREE!', 32, 2, RED)
    text2(img, bx - width('GPL-3.0', 20, 1) // 2, by + 22, 'GPL-3.0', 20, 1, BLACK)

    # --- tagline block next to the burst
    lines = [('ALL THIS SINCE 1.0:', TEXT),
             ('RECORD MIDI. AUTOMATE ANYTHING.', CYAN),
             ('INSERT FX ON EVERY TRACK.', GREEN),
             ('NEW SYNTHS ON THE CARD.', PURPLE)]
    ty = 392
    for s, c in lines:
        text2(img, 70, ty, s, 20, 2, c, shadow=(3, 3, BLACK))
        ty += 46

    # --- 2x2 screenshot windows, screenshots at 1:1
    titles = ['MIDI LANES + EVENTS', 'LEARN MAP', 'MIXER INSERTS', 'SONG TIMELINE']
    gx = [60, 940]
    gy = [672, 1196]
    for i, path in enumerate(SHOTS):
        sx, sy = gx[i % 2], gy[i // 2]
        inner = wb_window(img, (sx - 6, sy - 32, sx + 800 + 6, sy + 480 + 6), titles[i], active=(i == 0))
        shot = Image.open(path).convert('RGB')
        img.paste(shot, (sx, sy))

    # --- feature list window
    fy0 = 1714
    inner = wb_window(img, (60, fy0, W - 60, 2128), 'NEW SINCE 1.0.0 - 1.1 / 1.2 / 1.3')
    d.rectangle(inner, fill=PANE)
    col1 = [
        ('1.3', 'MIDI recording into lanes'),
        ('', 'chords, sub-step timing, CC'),
        ('1.3', 'automate any Learn parameter'),
        ('1.3', 'LEARN MAP screen'),
        ('1.3', 'TB-303 Chip FM SID on the card'),
        ('1.2', '2 insert FX per track + master'),
        ('1.2', '11 effect modules, sidechain'),
        ('1.2', '13 Sample Editor plugins'),
    ]
    col2 = [
        ('1.2', 'wavetable envelopes and LFOs'),
        ('1.2', 'MIDI Time Code out and in'),
        ('1.2', '20 step shuffles in Perform'),
        ('1.2', 'editable Song TIMELINE'),
        ('1.1', 'SID-style synth, stereo samples'),
        ('1.1', 'up to 10 USB audio channels'),
        ('1.1', 'update from the unit itself'),
        ('', 'and lots more in the changelog'),
    ]
    tag = {'1.3': ORANGE, '1.2': CYAN, '1.1': GREEN, '': DIM}
    for c, items in enumerate((col1, col2)):
        x = inner[0] + 24 + c * 830
        yy = inner[1] + 20
        for v, s in items:
            if v:
                d.rectangle((x, yy + 2, x + 50, yy + 34), fill=tag[v])
                text2(img, x + 1, yy + 2, v, 16, 2, BLACK)
            text2(img, x + 66, yy + 2, s, 16, 2, TEXT if v else DIM)
            yy += 45

    # --- footer floor + URL bar
    grid_floor(img, 2134, H)
    d.rectangle((0, 2178, W, 2236), fill=BLACK)
    url = 'GITHUB.COM/TRAK3R-OS/TRACK3R-OS'
    text2(img, 60, 2185, url, 20, 2, YELLOW)
    note = 'UNOFFICIAL. NOT AFFILIATED WITH POLYEND.'
    text2(img, W - 60 - width(note, 16, 1), 2200, note, 16, 1, DIM)

    img.save(OUT, optimize=True)
    print(OUT, img.size)


main()
