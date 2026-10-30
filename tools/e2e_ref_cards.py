#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The cards of the reference-player journeys (OS-E2E-9, docs/specs/OS-E2E.md "E2E-9"): small synthetic modules, generated, never committed.

  e2e_ref_cards.py --out DIR

writes, under DIR (the build's sim/cards, where `o3sim --cards` looks), one card folder for each journey of sim/scenarios/e2e/e2e_ref_*.txt:

  e2e_ref_mod/ref.mod      a ProTracker module: one channel of a looped 32-byte sine at period 428 (8,287 Hz: 259 Hz) and the effects both players run on every tick
  e2e_ref_s3m/ref.s3m      a Scream Tracker 3 module on the Modern engine's tracker FX (16-frame sine, C2SPD 8363)
  e2e_ref_med/ref.med      an OctaMED MMD1 module with the commands the MED tick processor and libopenmpt share
  e2e_ref_paula/paula.mod  a MOD whose one voice plays a bright saw at three periods: the Paula sound (band energy and aliasing)
  e2e_import_any/          OS-IMPORT-ANY (sim/scenarios/qa/qa_file.txt): mod.title (a MOD named by prefix), tune (an S3M with no type in its name) and README.txt (not a module)
  e2e_ref_export/tones.s3m an S3M of five pure tones from 689 Hz to 9.3 kHz, for the 44.1 kHz export (the resampler's gain, pitch and error against a band-limited reference)

Nothing in them is a real module: every note, name and sample is made up from tools/mod_build.py and tools/mmd_build.py (stdlib only; run with `python3 -I`). The same bytes are what
libopenmpt renders in tests/e2e_openmpt_ref.py: the test reads them back from the card folders, so the module and the reference cannot drift apart. The song of each card is described
here and in the header of the scenario that imports it.
"""
import math
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mmd_build as mm  # noqa: E402
import mod_build as mb  # noqa: E402

TICK_MS = 20       # speed 6 at 125 BPM, the default of the three formats
SPEED = 6


def sine_s8(n, amp=100):
    return mb.sine(n, amp)


def saw_s8(n, amp=40):
    """One cycle of a falling saw as signed 8-bit: the Paula journey's bright sample (every harmonic, 1 / k)."""
    return bytes(int(round(amp * (1.0 - 2.0 * i / n))) & 0xFF for i in range(n))


# ---- ProTracker -----------------------------------------------------------------------------------------------------------


def mod_ref():
    """Channel 1, seven cases of two rows (a row is 6 ticks of 20 ms: 240 ms a case), every case on a fresh C-2 (period 428: 259 Hz, sample 1) so that one case's difference does not carry into the next:
      rows  0- 1  arpeggio 037 on the note row and the row after: root, +3, +7 semitones a tick
      rows  2- 3  slide up 104 on both rows (period -4 a tick)
      rows  4- 5  fine slide up E13 on the note row (once, tick 0), nothing after
      rows  6- 7  volume slide down A03 on both rows (3 a tick of 64)
      rows  8- 9  the note at volume 32 (C20 beside it), then C10 alone on the next row (set the volume of the sounding note to 16)
      rows 10-11  tone portamento to G-2 (period 285): the G-2 row carries 305 (5 a tick), the row after 300 (the speed kept)
      rows 12-13  note cut EC3 on the note row (silent from tick 3)
    """
    m = mb.Mod("M.K.", 4, "e2e ref")
    m.samples = [mb.ModSample(sine_s8(32), "sine", 64, 0, 0, 16)] + [mb.ModSample() for _ in range(30)]
    m.orders = [0]
    c2 = 12
    pat = {
        (0, 0): (c2, 1, 0x0, 0x37),
        (1, 0): (None, 0, 0x0, 0x37),
        (2, 0): (c2, 1, 0x1, 0x04),
        (3, 0): (None, 0, 0x1, 0x04),
        (4, 0): (c2, 1, 0xE, 0x13),
        (6, 0): (c2, 1, 0xA, 0x03),
        (7, 0): (None, 0, 0xA, 0x03),
        (8, 0): (c2, 1, 0xC, 0x20),
        (9, 0): (None, 0, 0xC, 0x10),
        (10, 0): (c2, 1, 0xC, 0x40),
        (11, 0): (19, 0, 0x3, 0x05),
        (12, 0): (None, 0, 0x3, 0x00),
        (13, 0): (c2, 1, 0xE, 0xC3),
    }
    m.patterns = [pat]
    return mb.build(m)


def paula_mod():
    """The Paula journey's module: a saw of 64 frames (every harmonic, 1 / k) played at C-2 (period 428: the sample at 8,287 Hz, 129.5 Hz), C-3 (214: 259 Hz) and F-3 (160: 346 Hz), four rows (480 ms)
    each at speed 6 and 125 BPM. A saw's harmonics are all there is to a sample and hold, so what Paula adds (the images of its rate, what the BLEP does to them) stands out."""
    m = mb.Mod("M.K.", 4, "e2e paula")
    m.samples = [mb.ModSample(saw_s8(64), "saw", 64, 0, 0, 32)] + [mb.ModSample() for _ in range(30)]
    m.orders = [0]
    pat = {
        (0, 0): (12, 1, 0xC, 0x40),   # C-2 (428): 129.5 Hz
        (4, 0): (24, 1, 0xC, 0x40),   # C-3 (214): 259 Hz
        (8, 0): (29, 1, 0xC, 0x40),   # F-3 (160): 346 Hz
    }
    m.patterns = [pat]
    return mb.build(m)


# ---- Scream Tracker 3 ------------------------------------------------------------------------------------------------------


def sine_u8(n, amp=80):
    return bytes((128 + v) & 0xFF for v in mb.sine(n, amp))


def s3m_ref():
    """Channel 1, six cases of two rows on a fresh C-4 (note byte 0x40) of the instrument (32 frames of sine, C2SPD 11025: 344.5 Hz, not a Paula rate, so the sample is on the Modern engine
    whichever way the engine by rate goes), every case at volume 64 unless it says. S3M effects are letters (A = 1): D = 4, E = 5, F = 6, I = 9, J = 10.
      rows  0- 1  arpeggio J37 on the note row and the row after
      rows  2- 3  volume slide down D04 on both rows (4 a tick of 64 from tick 1)
      rows  4- 5  the note at volume 32, then D4F on the row after: the fine volume slide up 4, once at tick 0
      rows  6- 7  fine portamento up FF4 on the note row (once, tick 0)
      rows  8- 9  tremor I11 on the note row and the row after (2 ticks on, 2 off)
      rows 10-11  the note, then a volume of 16 in the volume column of a row with no note (libopenmpt sets the playing note's volume)
    """
    m = mb.S3m("e2e ref")
    m.instruments = [mb.S3mInst(sine_u8(32), "sine", 11025, 64, (0, 32))]
    m.orders = [0, 255]
    m.channels = [0, 8, 1, 9] + [255] * 28
    c4 = 0x40
    pat = {
        (0, 0): (c4, 1, 64, 10, 0x37),
        (1, 0): (255, 0, 255, 10, 0x37),
        (2, 0): (c4, 1, 64, 4, 0x04),
        (3, 0): (255, 0, 255, 4, 0x04),
        (4, 0): (c4, 1, 32, 0, 0),
        (5, 0): (255, 0, 255, 4, 0x4F),
        (6, 0): (c4, 1, 64, 6, 0xF4),
        (8, 0): (c4, 1, 64, 9, 0x11),
        (9, 0): (255, 0, 255, 9, 0x11),
        (10, 0): (c4, 1, 64, 0, 0),
        (11, 0): (255, 0, 16, 0, 0),
    }
    m.patterns = [pat]
    return mb.build(m)


def tones_s3m():
    """Five sines of 2 rows (240 ms) each at speed 6 and 125 BPM, the instrument 32 frames of sine at C2SPD 11025 (not a Paula rate): C-5 (689 Hz), C-6 (1,378), C-7 (2,756), C-8 (5,512) and A-8
    (9,267 Hz), then a pattern break so that the song ends after row 10. Volume 64; nothing else on the pattern."""
    m = mb.S3m("e2e tones")
    m.instruments = [mb.S3mInst(sine_u8(32), "sine", 11025, 64, (0, 32))]
    m.orders = [0, 255]
    m.channels = [0, 8, 1, 9] + [255] * 28
    pat = {}
    for i, note in enumerate((0x50, 0x60, 0x70, 0x80, 0x89)):
        pat[(2 * i, 0)] = (note, 1, 64, 0, 0)
    pat[(10, 1)] = (255, 0, 255, 3, 0)   # Cxx: break to row 0 of the next order, which is the end of the song
    m.patterns = [pat]
    return mb.build(m)


# ---- OctaMED ----------------------------------------------------------------------------------------------------------------


def med_ref():
    """MMD1, BPM mode (flags2 0x20 | 3: four lines a beat), speed 6, 125 BPM: a tick is 20 ms. Track 1, seven cases on a fresh note 13 (period 428, 259 Hz) of instrument 1 (a 32-byte sine):
      lines  0- 1  arpeggio 00 37 on the note line and the line after
      lines  2- 3  slide up 01 04 on both lines
      lines  4- 5  slide down 02 04 on both lines
      lines  6- 7  volume slide down 0A 03 on both lines (VOLHEX: 0C takes the volume as it is, 0..0x40; the slide is in the same units)
      lines  8- 9  the note at 0C 20 (volume 32), then 0C 10 alone on the next line (volume 16 for the sounding note)
      lines 10-12  tone portamento: the note, then note 20 with 03 05 (5 a tick), then 03 00 (the speed kept)
      lines 13-14  note cut 18 03 on the note line (silent from tick 3)
    """
    m = mm.Module(1)
    m.deftempo, m.tempo2 = 125, SPEED
    m.flags = 0x10          # VOLHEX (O10: libopenmpt reads it the same way)
    m.flags2 = 0x20 | 3     # BPM mode, four lines a beat
    b = mm.Block(4, 16)
    n = 13
    b.cell(0, 0, n, 1, 0x00, 0x37)
    b.cell(1, 0, 0, 0, 0x00, 0x37)
    b.cell(2, 0, n, 1, 0x01, 0x04)
    b.cell(3, 0, 0, 0, 0x01, 0x04)
    b.cell(4, 0, n, 1, 0x02, 0x04)
    b.cell(5, 0, 0, 0, 0x02, 0x04)
    b.cell(6, 0, n, 1, 0x0A, 0x03)
    b.cell(7, 0, 0, 0, 0x0A, 0x03)
    b.cell(8, 0, n, 1, 0x0C, 0x20)
    b.cell(9, 0, 0, 0, 0x0C, 0x10)
    b.cell(10, 0, n, 1, 0x0C, 0x40)
    b.cell(11, 0, 20, 0, 0x03, 0x05)
    b.cell(12, 0, 0, 0, 0x03, 0x00)
    b.cell(13, 0, n, 1, 0x18, 0x03)
    m.blocks = [b]
    m.playseq = [0]
    ins = mm.Instrument(type=0, data=sine_s8(32), name="sine")
    ins.rep, ins.replen = 0, 16
    m.instruments = [ins]
    return mm.build(m)


# ---- the cards -------------------------------------------------------------------------------------------------------------


def cards():
    """folder -> {file name: bytes}"""
    return {
        "e2e_ref_mod": {"ref.mod": mod_ref()},
        "e2e_ref_s3m": {"ref.s3m": s3m_ref()},
        "e2e_ref_med": {"ref.med": med_ref()},
        "e2e_ref_paula": {"paula.mod": paula_mod()},
        "e2e_ref_export": {"tones.s3m": tones_s3m()},
        "e2e_import_any": {"mod.title": mod_ref(), "tune": s3m_ref(), "README.txt": b"This is not a module.\n"},
    }


def write_all(out):
    for folder, files in cards().items():
        path = os.path.join(out, folder)
        shutil.rmtree(path, ignore_errors=True)
        os.makedirs(path)
        for name, data in files.items():
            with open(os.path.join(path, name), "wb") as f:
                f.write(data)


def main(argv):
    if len(argv) == 3 and argv[1] == "--out":
        write_all(argv[2])
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
