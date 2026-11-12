#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Promo cover for track3r-os v1.4.0 in the style of an early-90s computer magazine cover: a huge masthead,
# a starburst flash, a COVERDISK sticker, a hero picture, stacked cover lines with drop shadows, a strap line.
# An homage to the STYLE only: the masthead is our own project name, no real magazine's name or logo, no
# Polyend artwork. Everything is drawn pixel by pixel: the tracker's own bitmap fonts (Terminus, Spleen),
# its Amiga theme palette, real simulator screenshots at 1:1 (crops only), copper bars, ordered dithering.
# No smoothing.
#
# The cover lists everything new from v1.1 to v1.4, grouped by issue (the Highlights of docs/release/v1.1.0.md
# to v1.4.0.md).
#
# Usage: python3 -I docs/img/press/make_cover.py "$PWD" MIXER_EQ.png
# (one 800x480 simulator screenshot; the PNG is written next to this script). The shot is from o3sim: a scenario
# that opens the sixth Mixer page and sets the bands, then `snap`:
#   document / tap pattern / tap master / tap f1 / tap master 5 / encoder 8 / tap down / encoder 3 /
#   tap down / encoder -3 / tap down / encoder 5 / tap down / encoder 9 / tap down / encoder 40 / tap up
import sys, os, math
from PIL import Image, ImageDraw

ROOT = sys.argv[1]
SHOT_EQ = sys.argv[2]
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'track3r-os-v1.4.0-cover.png')

# The firmware's Amiga theme (ui/src/themes.cpp) plus a few 32-colour-era extras.
CHROME = (178, 186, 202); CHROME_TXT = (14, 18, 28); CHROME_DIM = (60, 70, 96)
PANE = (10, 13, 20); TEXT = (230, 234, 242); DIM = (128, 136, 156)
SEL = (28, 92, 208); ORANGE = (255, 138, 0); GREEN = (84, 200, 92); RED = (190, 30, 30)
WB_BLUE = (0, 85, 170); WHITE = (255, 255, 255); BLACK = (0, 0, 0)
YELLOW = (255, 221, 0); CYAN = (126, 196, 240); PURPLE = (212, 166, 238)
PINK = (255, 90, 170)


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


def cover_line(img, x, y, s, face=32, scale=1, color=YELLOW, plate=None, bullet=ORANGE):
    """One shouty cover line: optional colour plate, a diamond bullet, drop shadow."""
    d = ImageDraw.Draw(img)
    tw = width(s, face, scale)
    th = F[face][2] * scale
    if bullet:
        cx, cy = x + 12, y + th // 2
        d.polygon([(cx, cy - 11), (cx + 11, cy), (cx, cy + 11), (cx - 11, cy)], fill=BLACK)
        d.polygon([(cx, cy - 8), (cx + 8, cy), (cx, cy + 8), (cx - 8, cy)], fill=bullet)
        x += 34
    if plate:
        d.rectangle((x - 8 + 5, y - 2 + 5, x + tw + 8 + 5, y + th + 2 + 5), fill=BLACK)
        d.rectangle((x - 8, y - 2, x + tw + 8, y + th + 2), fill=plate)
        text2(img, x, y, s, face, scale, color)
    else:
        text2(img, x, y, s, face, scale, color, shadow=(3, 3, BLACK))
    return y + th


def disc(img, cx, cy, r):
    """The COVERDISK sticker: a round pixel disc with rings, hard edges."""
    d = ImageDraw.Draw(img)
    d.ellipse((cx - r + 7, cy - r + 7, cx + r + 7, cy + r + 7), fill=BLACK)
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=BLACK)
    d.ellipse((cx - r + 4, cy - r + 4, cx + r - 4, cy + r - 4), fill=ORANGE)
    d.ellipse((cx - r + 14, cy - r + 14, cx + r - 14, cy + r - 14), fill=YELLOW)
    d.ellipse((cx - r + 22, cy - r + 22, cx + r - 22, cy + r - 22), fill=RED)
    # a few concentric "data tracks" like the underside of a disc
    for k in range(3):
        rr = r - 30 - k * 5
        d.ellipse((cx - rr, cy - rr, cx + rr, cy + rr), outline=(150, 20, 20))


def crop_window(img, x, y, shot_path, crop, title):
    shot = Image.open(shot_path).convert('RGB').crop(crop)
    w, h = shot.size
    wb_window(img, (x - 6, y - 32, x + w + 6, y + h + 6), title)
    img.paste(shot, (x, y))
    return y + h + 6


def block(img, x, y, tag, title, col, lines, w=570):
    """One issue block: a colour plate with the issue number, then one shouty line each."""
    d = ImageDraw.Draw(img)
    hdr = tag + ' ' + title
    tw = width(hdr, 32, 1)
    d.rectangle((x + 5, y + 5, x + tw + 24 + 5, y + 37 + 5), fill=BLACK)
    d.rectangle((x, y, x + tw + 24, y + 37), fill=col)
    text2(img, x + 12, y + 3, hdr, 32, 1, BLACK)
    y += PLATE
    for i, s in enumerate(lines):
        assert 34 + width(s, 32, 1) <= w, s
        cover_line(img, x, y, s, 32, 1, WHITE if i % 2 else col, None, bullet=col)
        y += PITCH
    return y


PLATE, PITCH = 50, 37

LEFT, RIGHT = 20, 620
V14 = ['MST EQ: 5 BANDS + BASS BOOST', 'PIANO PADS: BEND + VIBRATO', 'MIDI PITCH WHEEL ON SAMPLES',
       '6 NEW SAMPLE EDITOR PLUGINS', 'SHIFT+FILE SAVES ANYWHERE', 'FOCUS LOOP + CHOP TO PATTERN',
       'MIDI LANES FOLLOW BLOCK EDITS', 'NEW PROJECTS START IN C MAJOR']
V13 = ['MIDI RECORDING INTO LANES', 'AUTOMATE ANY LEARN PARAMETER', 'LEARN MAP SCREEN',
       'STATUS LED BPM MODE', 'ADVANCED CHEAT SHEET']
V12L = ['INSERT FX: 2 PER TRACK + MASTER', '11 EFFECT MODULES + SIDECHAIN', 'MODULE PRESETS ON THE CARD',
        'RATE: CRUNCHY, CHEAPER MODULES', 'CHIP: 8 MORE WAVES', '13 SAMPLE EDITOR PLUGINS',
        'PATTERN TOOLS + APPS LIST']
V12R = ['WAVETABLE ENVELOPES + LFOS', 'PLAY THRU + CHANCE CONDITIONS', '20 CUSTOM PLAY ORDERS',
        'MIDI TIME CODE OUT + IN', 'MIDI EVENT LIST + CC MAP', 'EDITABLE SONG TIMELINE']
V11L = ['SID-STYLE SYNTH: 6581 + 8580', 'STEREO SAMPLES, 4 VOICES', 'USB AUDIO: UP TO 10 CHANNELS',
        'SONG: STAGE + TIMELINE PAGES']
V11R = ['SONG TOOLS: FORM + BRIDGE', 'IMPORT MODULES + SOUNDTRACKER', 'UPDATE SCREEN PADS OLD IMAGES']

Y_BLOCKS = 985
YA = Y_BLOCKS
YB = YA + PLATE + len(V14) * PITCH + 16
YC = YB + PLATE + len(V12L) * PITCH + 16
YD = YC + PLATE + len(V11L) * PITCH + 16      # coverdisk line
Y_STRAP = YD + 50 + 28


def main():
    W, H = 1200, Y_STRAP + 106
    img = Image.new('RGB', (W, H), PANE)
    d = ImageDraw.Draw(img)

    # --- background: dithered night sky, as many colours as a 32-colour screen could fake
    dither_band(img, (0, 0, W, 308), (0, 0, 60), (30, 40, 140))
    dither_band(img, (0, 308, W, Y_STRAP), (10, 14, 70), (70, 10, 100))

    # --- masthead: copper bars, raster-filled letters with a two-step 3D shadow
    copper(img, 40, 36, ramp((40, 0, 60), (255, 120, 255), 9))
    copper(img, 284, 36, ramp((0, 30, 60), (120, 220, 255), 9))
    text2(img, 40, 8, 'THE COMMUNITY OS MAGAZINE', 20, 2, YELLOW, shadow=(3, 3, BLACK))
    iss = 'ISSUE 1.4.0'
    text2(img, W - 40 - width(iss, 20, 2), 8, iss, 20, 2, WHITE, shadow=(3, 3, BLACK))

    logo = 'TRACK3R OS'
    sc = 7
    lx = (W - width(logo, 32, sc)) // 2
    rows = ramp((255, 255, 255), (110, 170, 255), 15) + [(20, 20, 60)] + ramp((255, 120, 0), (255, 240, 120), 16)

    def logo_fill(r, h):
        return rows[min(len(rows) - 1, r * len(rows) // h)]
    text_plain(img, lx + 14, 62 + 14, logo, *F[32][0:1], *F[32][1:3], sc, lambda r, h: BLACK)
    text_plain(img, lx + 7, 62 + 7, logo, *F[32][0:1], *F[32][1:3], sc, lambda r, h: (180, 20, 20))
    text2(img, lx, 62, logo, 32, sc, fill=logo_fill)

    # --- hero: the new MST EQ page at 1:1 in a window
    hx, hy = 300, 340
    wb_window(img, (hx - 6, hy - 32, hx + 806, hy + 486), 'MIXER: THE NEW MST EQ PAGE')
    img.paste(Image.open(SHOT_EQ).convert('RGB'), (hx, hy))

    # --- left stack beside the hero
    y = 330
    text2(img, 24, y, 'NEW!', 32, 3, YELLOW, shadow=(5, 5, RED))
    y += 102
    text2(img, 24, y, 'MASTER', 32, 2, WHITE, shadow=(4, 4, BLACK)); y += 60
    text2(img, 24, y, 'EQ!', 32, 2, CYAN, shadow=(4, 4, BLACK)); y += 70
    text2(img, 24, y, '5 BANDS +', 32, 1, GREEN, shadow=(3, 3, BLACK)); y += 38
    text2(img, 24, y, 'BASS BOOST', 32, 1, GREEN, shadow=(3, 3, BLACK)); y += 38
    text2(img, 24, y, 'IN THE CODEC', 32, 1, GREEN, shadow=(3, 3, BLACK)); y += 52
    d.rectangle((24 + 5, y + 5, 24 + 230 + 5, y + 40 + 5), fill=BLACK)
    d.rectangle((24, y, 24 + 230, y + 40), fill=RED)
    text2(img, 24 + 11, y + 4, 'ZERO CPU COST', 32, 1, WHITE)

    # --- starburst flash over the hero's top right corner
    bx, by = 1040, 372
    starburst(img, bx, by, 100, 148, 16, YELLOW, RED)
    text2(img, bx - width('FREE!', 32, 2) // 2, by - 44, 'FREE!', 32, 2, RED)
    text2(img, bx - width('V1.4.0', 20, 1) // 2, by + 26, 'V1.4.0', 20, 1, BLACK)

    # --- COVERDISK sticker under the left stack, left of the headline
    cx, cy = 150, 856
    disc(img, cx, cy, 108)
    text2(img, cx - width('FREE!', 20, 1) // 2, cy - 50, 'FREE!', 20, 1, WHITE)
    text2(img, cx - width('COVERDISK', 32, 1) // 2, cy - 16, 'COVERDISK', 32, 1, YELLOW, shadow=(2, 2, BLACK))
    text2(img, cx - width('ON THE CARD', 20, 1) // 2, cy + 28, 'ON THE CARD', 20, 1, WHITE)

    # --- the headline under the hero
    text2(img, 300, 846, 'EVERYTHING NEW SINCE 1.0!', 32, 2, YELLOW, shadow=(5, 5, RED))
    cover_line(img, 300, 922, '4 ISSUES IN ONE!  1.1 + 1.2 + 1.3 + 1.4', 32, 1, WHITE, RED, bullet=None)

    # --- the cover lines, one block per issue, newest first
    block(img, LEFT, YA, '1.4', 'THIS ISSUE', YELLOW, V14)
    yy = block(img, RIGHT, YA, '1.3', 'MIDI + AUTOMATION', ORANGE, V13)
    text2(img, RIGHT + 4, yy + 18, 'AND MUCH MORE: SEE THE', 20, 2, (190, 196, 214), shadow=(2, 2, BLACK))
    text2(img, RIGHT + 4, yy + 18 + 40, 'CHANGELOG FOR EVERY ISSUE!', 20, 2, (190, 196, 214), shadow=(2, 2, BLACK))
    block(img, LEFT, YB, '1.2', 'INSERT FX + PLUGINS', CYAN, V12L)
    block(img, RIGHT, YB, '1.2', 'AND MORE', CYAN, V12R)
    block(img, LEFT, YC, '1.1', 'SID + STEREO + USB', GREEN, V11L)
    block(img, RIGHT, YC, '1.1', 'AND MORE', GREEN, V11R)

    # --- the coverdisk line
    cover_line(img, 20, YD, 'FREE! COVERDISK:', 32, 1, YELLOW, RED, bullet=None)
    text2(img, 20 + width('FREE! COVERDISK:', 32, 1) + 34, YD, '29 MODULES + 19 PLUGINS + DEMO SONGS', 32, 1,
          WHITE, shadow=(3, 3, BLACK))

    # --- strap line
    d.rectangle((0, Y_STRAP, W, H), fill=BLACK)
    d.rectangle((0, Y_STRAP, W, Y_STRAP + 3), fill=ORANGE)
    strap = 'FREE COMMUNITY FIRMWARE FOR YOUR TRACKER - UNOFFICIAL'
    text2(img, (W - width(strap, 20, 2)) // 2, Y_STRAP + 18, strap, 20, 2, YELLOW, shadow=(3, 3, RED))
    note = 'NOT AFFILIATED WITH OR ENDORSED BY POLYEND.   GITHUB.COM/TRAK3R-OS/TRACK3R-OS   GPL-3.0'
    text2(img, (W - width(note, 16, 1)) // 2, Y_STRAP + 72, note, 16, 1, DIM)

    img.save(OUT, optimize=True)
    print(OUT, img.size, os.path.getsize(OUT))


main()
