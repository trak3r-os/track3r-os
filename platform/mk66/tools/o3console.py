#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""o3console: talk to the Tracker's `diag` console over USB serial (stdlib only; POSIX).

The unit enumerates as 16D0:0E9F "Track3r" with a CDC serial port; `diag` is the line console on it (platform/mk66/README.md,
"USB and the `diag` console"). This tool sends one command per argument, waits for the prompt `o3> ` after each and prints the reply:

    o3console.py --port /dev/ttyACM0 version usb "log 30"
    o3console.py --list                      # likely ports (by-id names containing Track3r, /dev/cu.usbmodem* on macOS)
    o3console.py --self-test                 # against a pseudo-terminal that plays the unit: no hardware

It never sends anything the console does not document. `reset` is passed through like any other command (it restarts the unit: SYSRESETREQ).
Exit status: 0 every reply complete, 1 a usage or port error, 2 a reply did not end with the prompt before the timeout.

The screen (OS-HIL; the device side is `shot`, usb/shot_stream.h, the decoder is o3shot.py in this directory):

    o3console.py --shot out.png [--shot-format 565|8888] [--shot-width N] [--shot-pclk0] [--shot-retries 2] ["key f1" ...]

asks the unit for its screen as framed base64 lines (a CRC on every line and on the whole picture), checks them, and writes out.png (RGB, stdlib only). Commands given as
well run first, in order. A picture whose lines were damaged, or during which the app drew a new frame (`moved=1`: the bands may belong to different pictures), is asked
for again, up to --shot-retries times. Exit status: 0 written, 2 no prompt or the transfer stalled, 4 the unit refused (`SHOT ERR ...`) or the data is not a whole picture.

Firmware update over USB (OS-006c; the device side is `fw` in platform/mk66/README.md, "Firmware update over USB"):

    o3console.py --put FILE.ptf [--name o3os-0.7.ptf] [--expect-volume ABCD-1234]
    o3console.py --update o3os-0.7.ptf --crc 1A2B3C4D --yes-update [--expect-volume ABCD-1234] [--expect-version TEXT]

`--put` first runs `check_image.py check` (this directory) on the file and refuses to send one that fails it, then sends `fw put <name> <bytes> <crc32>`
and the file in windows of 4096 bytes past the last acknowledgement the unit printed, and prints the unit's reply (`ok <name> <bytes> <crc> image <length>`,
or `ERR <reason> at <offset>`). `--update` runs `fw arm` and `fw update <token>` (the unit then restarts into the bootloader), waits for the port to disappear
and come back (up to --reboot-timeout seconds), runs `version` and prints it; `--expect-version` makes a version that does not contain the text a failure.
It needs `--yes-update` because it restarts the unit. `--expect-volume` is an optional check: it refuses both when the card's volume serial (`fw` reports it) is a different one.
Since OS-FWANY (maintainer decision 2026-10-13) and OS-ANYCARD (2026-10-17) the unit needs no marker file on the card and the tool needs no volume: any writable FAT card with a
Firmware/ folder takes `fw put`, whatever card is in the unit. The first install of an image that has `fw` goes the old way, by card.
Exit status of these: 0 done, 1 usage, 2 no prompt, 3 wrong card, 4 the unit refused or failed, 5 the unit did not come back, 6 version mismatch, 7 the bootloader did not take over.
"""
import argparse
import glob
import importlib.util
import os
import re
import select
import subprocess
import sys
import tempfile
import threading
import time
import zlib

PROMPT = b"o3> "


def find_ports():
    ports = sorted(glob.glob("/dev/serial/by-id/*Track3r*"))
    ports += sorted(glob.glob("/dev/cu.usbmodem*"))
    return ports


def open_port(path):
    import termios
    import tty

    fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
    try:
        tty.setraw(fd)
        attrs = termios.tcgetattr(fd)
        attrs[2] |= termios.CLOCAL | termios.CREAD  # a CDC port has no carrier to wait for
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
    except termios.error:
        pass  # a pseudo-terminal in the self-test may refuse some settings: raw mode above is enough
    return fd


def read_until_prompt(fd, timeout):
    """Bytes up to and including the prompt, or (bytes, False) if the timeout came first."""
    buf = b""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.05)
        if ready:
            chunk = os.read(fd, 4096)
            if not chunk:
                break
            buf += chunk
            if buf.endswith(PROMPT):
                return buf, True
    return buf, False


def drain(fd, quiet=0.15):
    """Discards what is queued until the line has been quiet for `quiet` seconds (a banner, an extra prompt)."""
    while True:
        ready, _, _ = select.select([fd], [], [], quiet)
        if not ready:
            return
        if not os.read(fd, 4096):
            return


def clean(raw, command):
    """The reply as text: CR LF to LF, the echo of the command and the trailing prompt removed."""
    text = raw.decode("utf-8", "replace").replace("\r\n", "\n").replace("\r", "\n")
    if text.endswith(PROMPT.decode()):
        text = text[: -len(PROMPT)]
    lines = text.split("\n")
    if lines and lines[0].strip() == command.strip():
        lines = lines[1:]
    return "\n".join(lines).rstrip("\n")


def run(fd, commands, timeout, out):
    # Wake the console: an empty line gives a fresh prompt whether or not the banner was seen (the banner comes when DTR rises).
    os.write(fd, b"\r")
    _, ok = read_until_prompt(fd, timeout)
    if not ok:
        print("o3console: no prompt from the unit within %.1f s" % timeout, file=sys.stderr)
        return 2
    drain(fd)  # the banner and the prompts that answered the wake-up: the first command starts on a quiet line
    status = 0
    for command in commands:
        os.write(fd, command.encode() + b"\r")
        raw, ok = read_until_prompt(fd, timeout)
        print(clean(raw, command), file=out)
        if not ok:
            print("o3console: no prompt after %r within %.1f s" % (command, timeout), file=sys.stderr)
            status = 2
            if command.strip() == "reset":
                status = 0  # the unit restarts: no prompt is the expected answer
    return status



# ---- firmware update over USB (OS-006c) ----------------------------------------------------------------------------------------

WINDOW = 4096  # bytes the host may have outstanding past the last `+N` acknowledgement (the unit's receive ring is sized for it)
NAME_RE = re.compile(r"^[0-9A-Za-z][0-9A-Za-z._-]{0,47}\.ptf$", re.IGNORECASE | re.ASCII)
EXPECT_SP = "0x20030000"
RAMFUNC_L = "0x1FFF0000:0xC000"


def valid_image_name(name):
    """The device's path guard, as far as a host can check it: [0-9A-Za-z][0-9A-Za-z._-]{0,47} then .ptf (any case), no '..'."""
    return bool(NAME_RE.fullmatch(name)) and ".." not in name


def local_check(path, out):
    """Runs check_image.py on the file (the gate the build runs). True when it passes."""
    tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), "check_image.py")
    run = subprocess.run([sys.executable, "-I", tool, "check", path, "--expect-sp", EXPECT_SP, "--ramfunc-l", RAMFUNC_L],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if run.returncode != 0:
        print(run.stdout, file=out)
        print("o3console: %s does not pass check_image.py: not sent" % path, file=out)
        return False
    return True


def exchange(fd, line, timeout):
    """One command: its reply as clean text, and whether the prompt came."""
    os.write(fd, line.encode() + b"\r")
    raw, ok = read_until_prompt(fd, timeout)
    return clean(raw, line), ok


_SIBLINGS = {}


def load_sibling(name):
    """Another tool of this directory as a module (python -I does not put the script's directory on the path)."""
    if name not in _SIBLINGS:
        spec = importlib.util.spec_from_file_location("o3_" + name, os.path.join(os.path.dirname(os.path.abspath(__file__)), name + ".py"))
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        _SIBLINGS[name] = module
    return _SIBLINGS[name]


def wake(fd, timeout):
    os.write(fd, b"\r")
    _, ok = read_until_prompt(fd, timeout)
    if ok:
        drain(fd)
    return ok


def check_volume(fd, expect, timeout, out):
    """0 when the unit's card has the volume serial `expect` (or none is expected), else the exit status to give."""
    if not expect:
        return 0
    reply, ok = exchange(fd, "fw", timeout)
    if not ok:
        print("o3console: no prompt after 'fw'", file=out)
        return 2
    match = re.search(r"volume ([0-9A-Fa-f]{4}-[0-9A-Fa-f]{4})", reply)
    if not match:
        print("o3console: the unit reports no card volume (is a card mounted?):\n%s" % reply, file=out)
        return 3
    if match.group(1).upper() != expect.upper():
        print("o3console: the card is volume %s, not %s: nothing was written" % (match.group(1), expect), file=out)
        return 3
    return 0


def put_file(fd, name, data, out, timeout=3.0, done_timeout=90.0):
    """`fw put`: 0 stored, 2 no prompt, 4 the unit refused or failed. Prints the unit's reply."""
    crc = zlib.crc32(data) & 0xFFFFFFFF
    command = "fw put %s %d %08X" % (name, len(data), crc)
    os.write(fd, command.encode() + b"\r")
    buf = b""
    deadline = time.monotonic() + timeout
    ready = re.compile(rb"(^|\n)ready\r?\n")
    while not ready.search(buf):
        if time.monotonic() > deadline:
            print("o3console: no answer to %r" % command, file=out)
            return 2
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            buf += os.read(fd, 4096)
            if buf.endswith(PROMPT) and not ready.search(buf):  # a refusal: one ERR line and the prompt
                print(clean(buf, command), file=out)
                return 4
    sent = 0
    acked = 0
    failed = False
    last_progress = time.monotonic()
    tail = buf[ready.search(buf).end():]
    buf = tail
    while sent < len(data) and not failed:
        r, w, _ = select.select([fd], [fd], [], 0.05)
        if r:
            chunk = os.read(fd, 4096)
            buf += chunk
            for found in re.finditer(rb"\+(\d+)\r?\n", buf):
                acked = max(acked, int(found.group(1)))
            if b"ERR " in buf:
                failed = True
                break
            last_progress = time.monotonic()
        if w and sent - acked < WINDOW:
            n = min(512, WINDOW - (sent - acked), len(data) - sent)
            sent += os.write(fd, data[sent:sent + n])
            last_progress = time.monotonic()
        if time.monotonic() - last_progress > 10.0:
            print("o3console: the unit stopped answering after %d of %d bytes" % (sent, len(data)), file=out)
            return 2
    if failed:
        # The unit discards the rest of the declared bytes (or waits 5 s of silence); it does not need them, and the prompt returns by itself.
        raw, ok = read_until_prompt(fd, 9.0)
        buf += raw
    else:
        raw, ok = read_until_prompt(fd, done_timeout)
        buf += raw
    text = buf.decode("utf-8", "replace").replace("\r\n", "\n").replace("\r", "\n")
    lines = [l for l in text.split("\n") if l.strip() and not re.fullmatch(r"\+\d+", l.strip()) and l.strip() != PROMPT.decode().strip()]
    print("\n".join(lines), file=out)
    if not ok:
        print("o3console: no prompt after the upload", file=out)
        return 2
    done = re.search(r"^ok (\S+) (\d+) ([0-9A-F]{8}) image (\d+)$", text, re.MULTILINE)
    if failed or not done:
        return 4
    if done.group(1) != name or int(done.group(2)) != len(data) or int(done.group(3), 16) != crc:
        print("o3console: the unit's `ok` line does not match what was sent", file=out)
        return 4
    return 0


def wait_gone(fd, timeout):
    """True when the port goes away (a read raises or says end of file), False if the unit stays and prints; returns also the text it printed."""
    deadline = time.monotonic() + timeout
    seen = b""
    while time.monotonic() < deadline:
        try:
            r, _, _ = select.select([fd], [], [], 0.05)
            if r:
                chunk = os.read(fd, 4096)
                if not chunk:
                    return True, seen
                seen += chunk
                if seen.endswith(PROMPT):  # the unit is up and said its piece: it did not leave
                    return False, seen
        except OSError:
            return True, seen
    return False, seen


def update_unit(fd, port, name, crc, args, out, find=None, opener=None, reboot_timeout=120.0, timeout=3.0):
    """`fw arm` + `fw update`, then waits for the unit to leave and come back and asks it `version`."""
    find = find or find_ports
    opener = opener or open_port
    status = check_volume(fd, args.expect_volume, timeout, out)
    if status:
        return status
    reply, ok = exchange(fd, "fw arm %s %08X" % (name, crc), max(timeout, 30.0))
    match = re.search(r"armed ([0-9A-F]{8})", reply)
    if not ok or not match:
        print(reply, file=out)
        print("o3console: the unit did not arm %s" % name, file=out)
        return 4 if ok else 2
    print(reply, file=out)
    os.write(fd, ("fw update %s\r" % match.group(1)).encode())
    gone, seen = wait_gone(fd, 8.0)
    text = seen.decode("utf-8", "replace").replace("\r\n", "\n")
    print(text.strip(), file=out)
    if not gone:
        if "ERR bootloader did not take over" in text:
            print("o3console: the bootloader did not take over (the unit is still running)", file=out)
            return 7
        if "ERR" in text:
            print("o3console: the unit refused the update", file=out)
            return 4
        # Nothing said yet: the unit's own 5 s wait may still end in its ERR line.
        more, seen2 = wait_gone(fd, 6.0)
        text2 = seen2.decode("utf-8", "replace").replace("\r\n", "\n")
        if not more:
            print(text2.strip(), file=out)
            print("o3console: the unit is still running after the hand-over" + ("" if "ERR" in text2 else " and said nothing"), file=out)
            return 7
    print("o3console: the unit left (hand-over to the bootloader); waiting for it to come back", file=out)
    os.close(fd)
    deadline = time.monotonic() + reboot_timeout
    new_fd = None
    while time.monotonic() < deadline and new_fd is None:
        for candidate in ([port] if os.path.exists(port) else []) + [p for p in find() if p != port]:
            try:
                new_fd = opener(candidate)
                break
            except OSError:
                continue
        time.sleep(0.5)
    if new_fd is None:
        print("o3console: the unit did not come back within %d s" % reboot_timeout, file=out)
        return 5
    try:
        for _ in range(20):  # the console may need a moment after enumeration
            if wake(new_fd, 1.5):
                break
        reply, ok = exchange(new_fd, "version", 5.0)
        print(reply, file=out)
        if not ok:
            return 2
        if args.expect_version and args.expect_version not in reply:
            print("o3console: the version does not contain %r" % args.expect_version, file=out)
            return 6
    finally:
        os.close(new_fd)
    return 0


# ---- the screen (OS-HIL) -------------------------------------------------------------------------------------------------------

def read_shot_text(fd, command, stall_timeout=10.0, total_timeout=120.0):
    """Sends `shot ...` and collects what the unit prints up to its prompt. (text, status): 0 whole, 2 stalled or too long."""
    os.write(fd, command.encode() + b"\r")
    buf = bytearray()
    started = last = time.monotonic()
    while True:
        r, _, _ = select.select([fd], [], [], 0.2)
        now = time.monotonic()
        if r:
            chunk = os.read(fd, 65536)
            if not chunk:
                return buf.decode("latin-1"), 2
            buf += chunk
            last = now
            if buf.endswith(PROMPT) and (b"SHOT END" in buf or b"SHOT ERR" in buf):
                return buf.decode("latin-1"), 0
        elif now - last > stall_timeout:
            return buf.decode("latin-1"), 2
        if now - started > total_timeout:
            return buf.decode("latin-1"), 2


def shot_unit(fd, path, out, fmt=None, width=None, retries=2, stall_timeout=10.0, pclk0=False):
    """`--shot`: the unit's screen into `path` as a PNG. 0 written, 2 no prompt or stalled, 4 refused or damaged. `pclk0`: the unit stops the panel clock while a band is taken (OS-SIMKEY2)."""
    shot_tool = load_sibling("o3shot")
    command = "shot" + ("" if fmt is None else " " + fmt + ("" if width is None else " %d" % width)) + (" pclk0" if pclk0 else "")
    last_problem = ""
    retries = max(0, min(int(retries), 5))  # a capped number of attempts, never a loop
    moving = None  # the last complete picture that was taken while the app redrew
    for attempt in range(retries + 1):
        text, status = read_shot_text(fd, command, stall_timeout)
        if status != 0:
            print("o3console: the unit stopped answering during `%s` (%d bytes arrived)" % (command, len(text)), file=out)
            return 2
        try:
            shot = shot_tool.parse_shot(text)
        except shot_tool.ShotError as error:
            print("o3console: %s" % error, file=out)
            return 4
        if not shot.complete:
            last_problem = "%d damaged lines, %d byte ranges missing" % (shot.bad_lines, len(shot.missing))
        elif shot.moved:
            last_problem = "the screen was redrawn while it was taken"
            moving = shot
        else:
            shot_tool.write_png(path, shot.width, shot.height, shot_tool.shot_rgb(shot))
            print("o3console: wrote %s (%dx%d, %s)" % (path, shot.width, shot.height, shot.fmt), file=out)
            return 0
        print("o3console: attempt %d of %d: %s" % (attempt + 1, retries + 1, last_problem), file=out)
    if moving is not None and shot.complete and shot.moved:  # every attempt was complete and every one moved: the UI animates (a playhead, a blink, a progress strip)
        shot_tool.write_png(path, moving.width, moving.height, shot_tool.shot_rgb(moving))
        print("o3console: UI animated, partial shot: the screen was redrawn during all %d attempts, so the bands may belong to different frames; wrote %s anyway "
              "(stop the transport and wait for the notification strip to clear, then ask again)" % (retries + 1, path), file=out)
        return 4
    print("o3console: no whole, still picture after %d attempts (%s)" % (retries + 1, last_problem), file=out)
    return 4


def shot_main(args, port):
    if port is None:
        print("o3console: give --port (or plug the unit in)", file=sys.stderr)
        return 1
    if args.shot_format not in (None, "565", "8888"):
        print("o3console: --shot-format is 565 or 8888", file=sys.stderr)
        return 1
    if getattr(args, "shot_pclk0", False) and args.shot_format is None:
        print("o3console: --shot-pclk0 needs --shot-format (the unit's command is `shot <format> [<w>] pclk0`)", file=sys.stderr)
        return 1
    if args.shot_width is not None and args.shot_format is None:
        print("o3console: --shot-width needs --shot-format (the unit's command is `shot <format> <w>`): it would be ignored", file=sys.stderr)
        return 1
    if args.shot_width is not None and not 1 <= args.shot_width <= 4095:
        print("o3console: --shot-width is 1..4095", file=sys.stderr)
        return 1
    try:
        fd = open_port(port)
    except OSError as error:
        print("o3console: cannot open %s: %s" % (port, error), file=sys.stderr)
        return 1
    try:
        if args.commands:
            status = run(fd, args.commands, args.timeout, sys.stdout)
            if status != 0:
                return status
        if not wake(fd, args.timeout):
            print("o3console: no prompt from the unit within %.1f s" % args.timeout, file=sys.stderr)
            return 2
        return shot_unit(fd, args.shot, sys.stdout, args.shot_format, args.shot_width, args.shot_retries, pclk0=getattr(args, "shot_pclk0", False))
    finally:
        os.close(fd)


# ---- self-test ---------------------------------------------------------------------------------------------------------------

class FakeUnit(threading.Thread):
    """Plays the console on the far side of a pseudo-terminal: echoes typing, answers a few commands, CR LF line ends, and (OS-006c) the `fw` protocol:
    `fw put` takes exactly the announced bytes raw with an acknowledgement every 2 KiB (or fails at byte `err_at` and discards the rest), `fw arm` answers
    a token, `fw update` says it hands over and then either vanishes (the master is closed) or, `stay`, says the bootloader did not take over."""

    REPLIES = {
        "version": "Track3r o3os, build test\nusb 16D0:0E9F rev 0001 serial 0123456789ABCDEF\n",
        "usb": "usb: Configured, address 7\n" + "bus: resets 1\n" * 40,  # long enough to need several reads
        "log 2": "log: 2 lines written, showing 2\n10 ms  boot: main reached\n20 ms  usb: configured\n",
    }

    def __init__(self, fd, volume="ABCD-1234", version="build test", err_at=None, stay=False, banner=True, shot_mode="ok"):
        super().__init__(daemon=True)
        self.shot_mode = shot_mode  # ok | moved_once | damaged_once | err | cut
        self.shots = 0
        self.fd = fd
        self.seen = []
        self.stop = False
        self.volume = volume
        self.err_at = err_at
        self.stay = stay
        self.banner = banner
        self.replies = dict(self.REPLIES)
        self.replies["version"] = "Track3r o3os, %s\nusb 16D0:0E9F rev 0001 serial 0123456789ABCDEF\n" % version
        self.raw_left = 0
        self.raw = bytearray()
        self.announced = None
        self.acked = 0
        self.discard = False
        self.last_raw = 0.0

    def say(self, text):
        os.write(self.fd, text.replace("\n", "\r\n").encode())

    def fw(self, args):
        if not args:
            self.say("fw: write path ENABLED\ncard: volume %s, FAT32, Firmware/ yes, write-protect no\n" % self.volume)
        elif args[0] == "put" and len(args) == 4:
            name, count, crc = args[1], int(args[2]), int(args[3], 16)
            if not valid_image_name(name):
                self.say("ERR name refused\n")
                return
            self.announced = (name, count, crc)
            self.raw_left, self.raw, self.acked, self.discard, self.last_raw = count, bytearray(), 0, False, time.monotonic()
            self.say("ready\n")
            self.prompt_pending = False
            return "raw"
        elif args[0] == "arm" and len(args) == 3:
            self.arm_name = args[1]
            self.say("armed 1A2B3C4D\n")
        elif args[0] == "update" and len(args) == 2 and args[1] == "1A2B3C4D":
            self.say("update: handing over to the bootloader (%s)\n" % self.arm_name)
            if self.stay:
                time.sleep(0.4)
                self.say("ERR bootloader did not take over\n")
            else:
                time.sleep(0.2)
                self.vanish()
                return "gone"
        else:
            self.say("ERR not armed for that token\n")

    def shot(self, args):
        """`shot [fmt [w]]`: a picture of a gradient in the device's framing (the text is made by o3shot's independent writer)."""
        shot_tool = load_sibling("o3shot")
        self.shots += 1
        fmt = args[0] if args else "565"
        bpp = 2 if fmt == "565" else 4
        data = (bytes(range(256)) * (800 * 480 * bpp // 256 + 1))[:800 * 480 * bpp]
        if self.shot_mode == "err":
            self.say("SHOT BEGIN fmt=%s w=800 h=480 bytes=%d line=96\nSHOT ERR snapshot timeout\n" % (fmt, len(data)))
            return
        moved = 1 if ((self.shot_mode == "moved_once" and self.shots == 1) or self.shot_mode == "moved_always") else 0
        damage = 5 if (self.shot_mode == "damaged_once" and self.shots == 1) else None
        text = shot_tool.make_capture(data, fmt, moved=moved, damage=damage, cut=(self.shot_mode == "cut"))
        self.picture = data
        os.write(self.fd, text.encode())

    def vanish(self):
        self.stop = True
        os.close(self.fd)

    def raw_byte(self, b):
        self.last_raw = time.monotonic()
        index = len(self.raw)
        self.raw.append(b)
        self.raw_left -= 1
        if self.discard:
            return
        if self.err_at is not None and index == self.err_at:
            self.say("ERR bad character at %d\n" % index)
            self.discard = True
            return
        if self.raw_left > 0 and len(self.raw) - self.acked >= 2048:
            self.say("+%d\n" % len(self.raw))
            self.acked = len(self.raw)
        if self.raw_left == 0:
            name, count, crc = self.announced
            self.say("ok %s %d %08X image %d\nfirst-ptf %s\n" % (name, count, zlib.crc32(bytes(self.raw)) & 0xFFFFFFFF, count // 2, name))
            os.write(self.fd, PROMPT)

    def run(self):
        line = b""
        if self.banner:
            os.write(self.fd, b"\r\nTrack3r diag - o3os test - 'help' lists the commands\r\n" + PROMPT)
        while not self.stop:
            try:
                ready, _, _ = select.select([self.fd], [], [], 0.05)
            except (OSError, ValueError):
                return
            if self.discard and self.raw_left > 0 and time.monotonic() - self.last_raw > 0.4:
                self.raw_left = 0  # the host stopped sending after the ERR: the unit's idle timeout ends the discard
                os.write(self.fd, PROMPT)
            if not ready:
                continue
            try:
                data = os.read(self.fd, 4096)
            except OSError:
                return
            for byte in data:
                if self.raw_left > 0:
                    self.raw_byte(byte)
                    continue
                ch = bytes([byte])
                if ch in (b"\r", b"\n"):
                    os.write(self.fd, b"\r\n")
                    command = line.decode()
                    line = b""
                    if command:
                        self.seen.append(command)
                    words = command.split()
                    if words and words[0] == "shot":
                        self.shot(words[1:])
                        os.write(self.fd, PROMPT)
                        continue
                    if words and words[0] == "fw":
                        outcome = self.fw(words[1:])
                        if outcome == "raw":
                            continue
                        if outcome == "gone":
                            return
                        os.write(self.fd, PROMPT)
                        continue
                    reply = self.replies.get(command)
                    if command == "":
                        reply = ""
                    elif reply is None:
                        reply = "unknown command: %s (help lists the commands)\n" % command
                    os.write(self.fd, reply.replace("\n", "\r\n").encode() + PROMPT)
                else:
                    line += ch
                    os.write(self.fd, ch)  # the echo


class Namespace:
    def __init__(self, **kw):
        self.expect_volume = None
        self.expect_version = None
        self.__dict__.update(kw)


def selftest_image(bad_config=False):
    """A HEX file that passes (or, with bad_config, fails) check_image.py: its own self-test's good image."""
    spec = importlib.util.spec_from_file_location("check_image_selftest", os.path.join(os.path.dirname(os.path.abspath(__file__)), "check_image.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    segments = module.good_segments()
    if bad_config:
        segments[module.CONFIG_ADDRESS] = bytes.fromhex("ffffffffffffffffffffffffdcf9ffff")
    # a longer image so that the upload needs several acknowledgements; it ends on a flash sector boundary (check_image `sector_end`)
    code = segments[module.CONFIG_ADDRESS + module.CONFIG_SIZE]
    segments[module.CONFIG_ADDRESS + module.CONFIG_SIZE] = (code * 20)[:2 * module.FLASH_SECTOR - module.CONFIG_ADDRESS - module.CONFIG_SIZE]
    return module.make_hex(segments).encode("ascii")


def pty_pair():
    master, slave = os.openpty()
    return master, slave, os.ttyname(slave)


def self_test_fw(problems):
    import io

    name = "o3os-0.7.ptf"
    with tempfile.TemporaryDirectory() as tmp:
        good = os.path.join(tmp, name)
        with open(good, "wb") as f:
            f.write(selftest_image())
        bad = os.path.join(tmp, "o3os-0.8.ptf")
        with open(bad, "wb") as f:
            f.write(selftest_image(bad_config=True))
        data = selftest_image()
        # the name rule
        for ok_name in (name, "Track3r_0.3.2.ptf", "x.PTF", "a-b_c.ptf", "A" * 48 + ".ptf"):
            if not valid_image_name(ok_name):
                problems.append("name %r refused" % ok_name)
        for bad_name in (".ptf", "a b.ptf", "a..b.ptf", "a/b.ptf", "../a.ptf", ".a.ptf", "a.txt", "A" * 49 + ".ptf"):
            if valid_image_name(bad_name):
                problems.append("name %r accepted" % bad_name)
        # the local gate: a good image passes, a bad flash configuration is never sent
        quiet = io.StringIO()
        if not local_check(good, quiet):
            problems.append("check_image refused the good image: %s" % quiet.getvalue())
        refused = io.StringIO()
        if local_check(bad, refused) or "not sent" not in refused.getvalue():
            problems.append("check_image accepted a bad flash configuration")

        # a good put: the bytes arrive complete and the ok line is accepted
        master, slave, path = pty_pair()
        unit = FakeUnit(master)
        unit.start()
        fd = open_port(path)
        out = io.StringIO()
        wake(fd, 2.0)
        status = put_file(fd, name, data, out)
        text = out.getvalue()
        if status != 0 or "ok %s %d" % (name, len(data)) not in text:
            problems.append("put: status %d: %s" % (status, text))
        if bytes(unit.raw) != data:
            problems.append("put: the unit received %d bytes, %s" % (len(unit.raw), "different" if unit.raw else "none"))
        if "ok %s" % name not in text:
            problems.append("put: the ok line was not printed")
        if re.search(r"^\+\d+$", text, re.MULTILINE):
            problems.append("put: an acknowledgement line was printed to the user")
        # the same unit still answers commands afterwards
        reply, ok = exchange(fd, "version", 2.0)
        if not ok or "build test" not in reply:
            problems.append("put: the console did not return to commands")
        unit.stop = True

        # an error injected at byte 3000: ERR is printed, the rest is not needed, status 4, the prompt comes back
        master, slave, path2 = pty_pair()
        unit2 = FakeUnit(master, err_at=3000)
        unit2.start()
        fd2 = open_port(path2)
        out2 = io.StringIO()
        wake(fd2, 2.0)
        status2 = put_file(fd2, name, data, out2)
        if status2 != 4 or "ERR bad character at 3000" not in out2.getvalue():
            problems.append("put with an injected ERR: status %d: %s" % (status2, out2.getvalue()))
        reply, ok = exchange(fd2, "version", 2.0)
        if not ok:
            problems.append("put with an injected ERR: the console did not come back")
        unit2.stop = True

        # a refused name (the unit says so)
        master, slave, path3 = pty_pair()
        unit3 = FakeUnit(master)
        unit3.start()
        fd3 = open_port(path3)
        wake(fd3, 2.0)
        out3 = io.StringIO()
        status3 = put_file(fd3, "a..b.ptf", data, out3)
        if status3 != 4 or "ERR name refused" not in out3.getvalue():
            problems.append("put with a refused name: status %d: %s" % (status3, out3.getvalue()))
        unit3.stop = True

        # the volume check: the wrong card is refused before anything is sent, the right one passes
        master, slave, path4 = pty_pair()
        unit4 = FakeUnit(master, volume="1111-2222")
        unit4.start()
        fd4 = open_port(path4)
        wake(fd4, 2.0)
        out4 = io.StringIO()
        if check_volume(fd4, "ABCD-1234", 2.0, out4) != 3 or "1111-2222" not in out4.getvalue():
            problems.append("a different card volume was not refused")
        if check_volume(fd4, "1111-2222", 2.0, io.StringIO()) != 0 or check_volume(fd4, None, 2.0, io.StringIO()) != 0:
            problems.append("the right volume was refused")
        if any(c.startswith("fw put") for c in unit4.seen):
            problems.append("a put was sent to the wrong card")
        unit4.stop = True

        # the update: arm, update, the port vanishes, a new unit appears and answers `version`
        master, slave, path5 = pty_pair()
        unit5 = FakeUnit(master)
        unit5.start()
        fd5 = open_port(path5)
        wake(fd5, 2.0)
        master_b, slave_b, path_b = pty_pair()
        unit_b = FakeUnit(master_b, version="build NEW")
        unit_b.start()
        out5 = io.StringIO()
        status5 = update_unit(fd5, path5, name, 0x12345678, Namespace(expect_volume="ABCD-1234", expect_version="build NEW"), out5,
                              find=lambda: [path_b], reboot_timeout=10.0, timeout=2.0)
        if status5 != 0 or "build NEW" not in out5.getvalue() or "armed 1A2B3C4D" not in out5.getvalue():
            problems.append("update: status %d: %s" % (status5, out5.getvalue()))
        if "fw arm %s 12345678" % name not in unit5.seen or "fw update 1A2B3C4D" not in unit5.seen:
            problems.append("update: the unit saw %r" % unit5.seen)
        unit_b.stop = True
        # the wrong version
        master, slave, path6 = pty_pair()
        unit6 = FakeUnit(master)
        unit6.start()
        fd6 = open_port(path6)
        wake(fd6, 2.0)
        master_c, slave_c, path_c = pty_pair()
        unit_c = FakeUnit(master_c, version="build OLD")
        unit_c.start()
        status6 = update_unit(fd6, path6, name, 0x12345678, Namespace(expect_version="build NEW"), io.StringIO(), find=lambda: [path_c], reboot_timeout=10.0, timeout=2.0)
        if status6 != 6:
            problems.append("update with the wrong version: status %d" % status6)
        unit_c.stop = True
        # the bootloader does not take over
        master, slave, path7 = pty_pair()
        unit7 = FakeUnit(master, stay=True)
        unit7.start()
        fd7 = open_port(path7)
        wake(fd7, 2.0)
        out7 = io.StringIO()
        status7 = update_unit(fd7, path7, name, 0x12345678, Namespace(), out7, find=lambda: [], reboot_timeout=1.0, timeout=2.0)
        if status7 != 7 or "did not take over" not in out7.getvalue():
            problems.append("update when the bootloader does not take over: status %d: %s" % (status7, out7.getvalue()))
        unit7.stop = True
        # the unit never comes back
        master, slave, path8 = pty_pair()
        unit8 = FakeUnit(master)
        unit8.start()
        fd8 = open_port(path8)
        wake(fd8, 2.0)
        status8 = update_unit(fd8, path8, name, 0x12345678, Namespace(), io.StringIO(), find=lambda: [], reboot_timeout=1.5, timeout=2.0)
        if status8 != 5:
            problems.append("update when the unit does not come back: status %d" % status8)
        unit8.stop = True
        # OS-ANYCARD: --put / --update need no --expect-volume: without it they go as far as the port (which does not exist here), and say nothing about an unidentified card
        import argparse as _ap
        base = dict(put=good, update=None, name=None, yes_update=True, crc="12345678", timeout=1.0, reboot_timeout=1.0, expect_volume=None, expect_version=None)
        saved_err, sys.stderr = sys.stderr, io.StringIO()
        try:
            for kind in ("put", "update"):
                kw = dict(base)
                if kind == "update":
                    kw.update(put=None, update=name)
                for volume in (None, "ABCD-1234"):
                    kw.update(expect_volume=volume)
                    rc = firmware_main(_ap.Namespace(**kw), "/nonexistent/o3-selftest")
                    if rc != 1 or "cannot open" not in sys.stderr.getvalue() or "--expect-volume" in sys.stderr.getvalue() or "WARNING" in sys.stderr.getvalue():
                        problems.append("%s with expect_volume=%r did not go as far as the port without a volume complaint (%d: %s)" % (kind, volume, rc, sys.stderr.getvalue()))
                    sys.stderr.truncate(0)
                    sys.stderr.seek(0)
        finally:
            sys.stderr = saved_err
        for descriptor in (fd, fd2, fd3, fd4):
            os.close(descriptor)


def self_test_shot(problems):
    import io

    shot_tool = load_sibling("o3shot")
    with tempfile.TemporaryDirectory() as tmp:
        for mode, fmt, wanted_status, wanted_shots in (("ok", None, 0, 1), ("ok", "8888", 0, 1), ("moved_once", None, 0, 2), ("damaged_once", None, 0, 2), ("err", None, 4, 1), ("cut", None, 2, 1),
                                                       ("moved_always", None, 4, 2)):
            master, slave = os.openpty()
            unit = FakeUnit(master, shot_mode=mode)
            unit.start()
            fd = open_port(os.ttyname(slave))
            wake(fd, 2.0)
            out = io.StringIO()
            path = os.path.join(tmp, "%s_%s.png" % (mode, fmt))
            status = shot_unit(fd, path, out, fmt=fmt, retries=1, stall_timeout=1.0 if mode == "cut" else 15.0)
            label = "shot %s %s" % (mode, fmt)
            if status != wanted_status or unit.shots != wanted_shots:
                problems.append("%s: status %d (%d wanted), %d requests (%d wanted): %s" % (label, status, wanted_status, unit.shots, wanted_shots, out.getvalue()))
            elif wanted_status == 0:
                w, h, rgb = shot_tool.read_png(path)
                expect = shot_tool.shot_rgb(shot_tool.parse_shot(shot_tool.make_capture(unit.picture, fmt or "565")))
                if (w, h) != (800, 480) or rgb != expect:
                    problems.append("%s: the PNG is not the picture the unit sent" % label)
            if status == 4 and mode == "err" and "snapshot timeout" not in out.getvalue():
                problems.append("shot err: the unit's reason was not printed")
            if mode == "moved_always":  # every attempt moved: capped, said plainly, the last picture kept for a look
                if "UI animated, partial shot" not in out.getvalue() or not os.path.exists(path):
                    problems.append("%s: not reported as an animated UI with the picture kept: %s" % (label, out.getvalue()))
            elif wanted_status != 0 and os.path.exists(path):
                problems.append("%s: a PNG was written for a failed shot" % label)
            if status == 0 or mode in ("err", "moved_always"):  # the console is back at its prompt
                reply, ok = exchange(fd, "version", 2.0)
                if not ok or "build test" not in reply:
                    problems.append("%s: the console did not return to commands" % label)
            unit.stop = True
            unit.join(2.0)  # before the descriptors go: a thread still selecting on a closed master's number would read the next pseudo-terminal's input
            os.close(fd)
            os.close(master)
            os.close(slave)
        # the number of attempts is capped (5 retries at most, whatever is asked) and says so; a move-free picture needs no retry
        master, slave = os.openpty()
        unit = FakeUnit(master, shot_mode="moved_always")
        unit.start()
        fd = open_port(os.ttyname(slave))
        wake(fd, 2.0)
        out = io.StringIO()
        status = shot_unit(fd, os.path.join(tmp, "capped.png"), out, retries=99, stall_timeout=15.0)
        if status != 4 or unit.shots != 6 or "UI animated, partial shot" not in out.getvalue():
            problems.append("shot retries were not capped at 5: status %d, %d requests" % (status, unit.shots))
        unit.stop = True
        unit.join(2.0)
        os.close(fd)
        os.close(master)
        os.close(slave)
    # --shot-width without --shot-format, and either without --shot, are errors and not silently ignored
    import argparse as _ap
    saved_err, sys.stderr = sys.stderr, io.StringIO()
    try:
        base = dict(shot="x.png", shot_format=None, shot_width=800, shot_retries=2, commands=[], timeout=1.0)
        if shot_main(_ap.Namespace(**base), "/nonexistent/o3-selftest") != 1 or "--shot-width needs --shot-format" not in sys.stderr.getvalue():
            problems.append("--shot-width without --shot-format was not an error: %s" % sys.stderr.getvalue())
        sys.stderr.truncate(0)
        sys.stderr.seek(0)
        for bad_width in (0, 4096):
            if shot_main(_ap.Namespace(**dict(base, shot_format="565", shot_width=bad_width)), "/nonexistent/o3-selftest") != 1:
                problems.append("--shot-width %d was accepted" % bad_width)
        if shot_main(_ap.Namespace(**dict(base, shot_format="565", shot_width=1600)), "/nonexistent/o3-selftest") != 1 or "cannot open" not in sys.stderr.getvalue():
            problems.append("a good --shot-format / --shot-width pair did not reach the port")
        for argv in (["--shot-width", "800"], ["--shot-format", "565", "--port", "/nonexistent"], ["--shot-width", "800", "--shot-format", "565", "--port", "/nonexistent"]):
            sys.stderr.truncate(0)
            sys.stderr.seek(0)
            if main(argv) != 1 or "only go with --shot" not in sys.stderr.getvalue():
                problems.append("%r without --shot was not an error: %s" % (argv, sys.stderr.getvalue()))
    finally:
        sys.stderr = saved_err


def self_test():
    import io

    master, slave = os.openpty()
    unit = FakeUnit(master)
    unit.start()
    fd = open_port(os.ttyname(slave))
    out = io.StringIO()
    status = run(fd, ["version", "usb", "log 2", "bogus"], 2.0, out)
    unit.stop = True
    text = out.getvalue()
    problems = []
    if status != 0:
        problems.append("status %d" % status)
    if "serial 0123456789ABCDEF" not in text:
        problems.append("version reply missing")
    if text.count("bus: resets 1") != 40:
        problems.append("long reply cut: %d lines" % text.count("bus: resets 1"))
    if "20 ms  usb: configured" not in text:
        problems.append("log reply missing")
    if "unknown command: bogus" not in text:
        problems.append("unknown-command reply missing")
    if "\r" in text:
        problems.append("a CR was left in the output")
    if "o3> " in text:
        problems.append("the prompt was left in the output")
    for command in ("version", "usb", "log 2", "bogus"):
        if command not in unit.seen:
            problems.append("the unit never saw %r" % command)
    # A silent unit must time out with status 2, not hang.
    master2, slave2 = os.openpty()
    fd2 = open_port(os.ttyname(slave2))
    err = io.StringIO()
    saved, sys.stderr = sys.stderr, err
    try:
        silent_status = run(fd2, ["version"], 0.3, io.StringIO())
    finally:
        sys.stderr = saved
    if silent_status != 2 or "no prompt" not in err.getvalue():
        problems.append("a silent unit did not give status 2")
    for descriptor in (fd, fd2, master, slave, master2, slave2):
        os.close(descriptor)
    self_test_fw(problems)
    self_test_shot(problems)
    if problems:
        print("o3console self-test FAILED: " + "; ".join(problems), file=sys.stderr)
        return 1
    print("o3console self-test passed")
    return 0


def firmware_main(args, port):
    """--put and --update. Returns the exit status documented at the top of the file."""
    if args.put and args.update:
        print("o3console: --put and --update are two runs", file=sys.stderr)
        return 1
    if args.put:
        name = args.name or os.path.basename(args.put)
        if not valid_image_name(name):
            print("o3console: %r is not a name the unit accepts (<1..48 of 0-9A-Za-z._-, first alphanumeric>.ptf, no '..'): give --name" % name, file=sys.stderr)
            return 1
        try:
            with open(args.put, "rb") as f:
                data = f.read()
        except OSError as error:
            print("o3console: cannot read %s: %s" % (args.put, error), file=sys.stderr)
            return 1
        if not data or len(data) > 4 * 1024 * 1024:
            print("o3console: %s is empty or over 4 MiB" % args.put, file=sys.stderr)
            return 1
        if not local_check(args.put, sys.stdout):
            return 4
    else:
        name = args.update
        if not valid_image_name(name):
            print("o3console: %r is not an image name of the form <stem>.ptf" % name, file=sys.stderr)
            return 1
        if not args.yes_update:
            print("o3console: --update restarts the unit into the bootloader: add --yes-update", file=sys.stderr)
            return 1
        if not args.crc or not re.fullmatch(r"[0-9A-Fa-f]{1,8}", args.crc):
            print("o3console: --update needs --crc <the file's CRC-32 in hex> (the `ok` line of the put prints it)", file=sys.stderr)
            return 1
    if port is None:
        print("o3console: give --port (or plug the unit in)", file=sys.stderr)
        return 1
    try:
        fd = open_port(port)
    except OSError as error:
        print("o3console: cannot open %s: %s" % (port, error), file=sys.stderr)
        return 1
    try:
        if not wake(fd, args.timeout):
            print("o3console: no prompt from the unit within %.1f s" % args.timeout, file=sys.stderr)
            return 2
        if args.put:
            status = check_volume(fd, args.expect_volume, args.timeout, sys.stdout)
            return status or put_file(fd, name, data, sys.stdout, args.timeout)
        status = update_unit(fd, port, name, int(args.crc, 16), args, sys.stdout, reboot_timeout=args.reboot_timeout, timeout=args.timeout)
        fd = None  # update_unit closed it when the unit left
        return status
    finally:
        if fd is not None:
            try:
                os.close(fd)
            except OSError:
                pass


def main(argv=None):
    parser = argparse.ArgumentParser(description="Talk to the Tracker's diag console over USB serial.")
    parser.add_argument("commands", nargs="*", help="commands to send, one per argument (quote those with arguments)")
    parser.add_argument("--port", help="serial device (default: the first --list entry)")
    parser.add_argument("--timeout", type=float, default=2.0, help="seconds to wait for each reply (default 2)")
    parser.add_argument("--list", action="store_true", help="list likely ports and exit")
    parser.add_argument("--self-test", action="store_true", help="run against a pseudo-terminal (no hardware) and exit")
    parser.add_argument("--put", metavar="FILE.ptf", help="send a firmware image to Firmware/ on the unit's card (checked with check_image.py first; any writable FAT card in the unit takes it)")
    parser.add_argument("--name", help="the file name on the card for --put (default: the file's name)")
    parser.add_argument("--update", metavar="NAME", help="arm and start the hand-over to the bootloader for Firmware/NAME (needs --crc and --yes-update)")
    parser.add_argument("--crc", help="CRC-32 of NAME's file in hex, as the put printed it (for --update)")
    parser.add_argument("--yes-update", action="store_true", help="confirm that --update restarts the unit into the bootloader")
    parser.add_argument("--expect-volume", metavar="XXXX-XXXX", help="optional: refuse --put / --update unless the card's volume serial is this (any SD card may be written, OS-ANYCARD)")
    parser.add_argument("--expect-version", metavar="TEXT", help="after --update: fail unless `version` contains TEXT")
    parser.add_argument("--shot", metavar="OUT.png", help="take the unit's screen into a PNG (the `shot` command, decoded and checked; see the text at the top)")
    parser.add_argument("--shot-format", metavar="565|8888", help="the snapshot format the unit is asked for (default 565)")
    parser.add_argument("--shot-pclk0", action="store_true", help="the unit stops the panel clock while it takes the picture (the Guide's CMD_SNAPSHOT2 recipe; OS-SIMKEY2: tests whether rectangles read at 0.545 of their colour because the clock runs); with --shot-format")
    parser.add_argument("--shot-width", type=int, metavar="N", help="the width CMD_SNAPSHOT2 is given (the unit's default is 800 for 565 and 1600 for 8888; an image from before OS-SIMKEY (0.4.8, 0.4.9) uses 800 for both, which returns half a screen in 8888: pass 1600; with --shot-format)")
    parser.add_argument("--shot-retries", type=int, default=2, help="how many times a damaged or moving picture is asked for again (default 2)")
    parser.add_argument("--reboot-timeout", type=float, default=120.0, help="seconds to wait for the unit to come back after --update (default 120)")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.list:
        for port in find_ports():
            print(port)
        return 0
    port = args.port or (find_ports() or [None])[0]
    if args.put or args.update:
        return firmware_main(args, port)
    if (args.shot_format is not None or args.shot_width is not None) and not args.shot:
        print("o3console: --shot-format / --shot-width only go with --shot", file=sys.stderr)
        return 1
    if args.shot:
        return shot_main(args, port)
    if port is None or not args.commands:
        parser.print_usage(sys.stderr)
        print("o3console: give --port (or plug the unit in) and at least one command", file=sys.stderr)
        return 1
    try:
        fd = open_port(port)
    except OSError as error:
        print("o3console: cannot open %s: %s" % (port, error), file=sys.stderr)
        return 1
    try:
        return run(fd, args.commands, args.timeout, sys.stdout)
    finally:
        os.close(fd)


if __name__ == "__main__":
    sys.exit(main())
