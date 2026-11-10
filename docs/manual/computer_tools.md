<!-- manual-sync: 32c3e0c -->
<!--
Chapter: Computer tools. Rules: .claude/skills/manual-writing/SKILL.md. Scope: the three USB tools for a computer (o3console, o3shot, o3audio) and the diag console commands a user needs. Not covered: o3hil (developer tool).
Sources: platform/mk66/tools/o3console.py, o3shot.py, o3audio.py (module docstrings and --help), platform/mk66/usb/include/o3os/platform/mk66/usb/command_table.h (diag command names and help lines), docs/manual/install.md (the --put and optional --expect-volume steps, the file name rule, the reset-button rule), platform/mk66/README.md ("USB and the diag console").
Manifest lines needed: none (docs/manual/images.txt does not exist yet; this chapter has no picture).
Open rows, not written: none.
-->
# Computer tools

Status: draft

Three Python tools on your computer talk to the unit over USB. `o3console.py` reads the screen, runs diagnostic commands and sends a firmware file to the card. `o3audio.py` measures the audio, and `o3shot.py` turns a saved text into a screen picture. They are for testing, for reporting a problem and for updating without taking the card out. New in track3r-os. <!-- og: new -->

## What it is for

The unit has a USB serial port. It runs a text console, the diag console, and it presents an audio device when USB AUDIO is on. Three Python tools on your computer use these. They read the screen, read the diagnostics, send a firmware file to the card and measure the audio. Use them when you test or report a problem.

Two tools are for a developer: `o3hil.py` runs the hardware tests and is not covered here.

## Set up Python on Linux

These tools are written for Linux. Windows and macOS are **untested**.

1. Open a terminal and run `python3 --version`. If it prints a version number, Python 3 is installed.
2. If the command is missing, install Python 3 with your package manager. On Debian and Ubuntu: `sudo apt install python3`.
3. The tools use only the standard library of Python. You install nothing else.
4. For `o3audio.py`, also install the audio recording program: `sudo apt install alsa-utils`.
5. Get a copy of the repository from GitHub. The tools need a file called `check_image.py` from the same folder, so run them from the repository folder.

Every command in this chapter starts with `python3 -I`. The `-I` option runs Python in isolated mode, as [Installing track3r-os](install.md) does. It is always the same, so you can copy the commands as they are.

To check a tool, run its self-test. The self-test needs no unit:

```
python3 -I platform/mk66/tools/o3console.py --self-test
```

## Connect the unit

1. Connect the unit to the computer with a USB cable. Turn the unit on.
2. List the likely ports: `python3 -I platform/mk66/tools/o3console.py --list`.
3. The unit shows up as a serial port called Track3r. On Linux the name is often `/dev/ttyACM0`.

If the tool reports a permission error on the port, your user may need access to serial ports. <!-- TODO: ask: which group the serial port needs on Linux. Not in the sources. (code: no answer) -->

## The diag console: o3console.py
<!-- sync: src=platform/mk66/tools/o3console.py -->

The console takes commands. You give each command as one argument of the tool. Quote a command that has a space in it.

```
python3 -I platform/mk66/tools/o3console.py --port /dev/ttyACM0 version "log 30"
```

The tool sends each command, waits for the prompt `o3> ` and prints the reply. It has no interactive mode. Use `--timeout` to wait longer for a reply; the default is 2 seconds.

The exit status tells you what happened: `0` means every reply was complete, `1` means a usage or port error, and `2` means a reply did not end with the prompt before the timeout.

### The commands you can use
<!-- sync: src=platform/mk66/usb/src/console.cpp,platform/mk66/usb/include/o3os/platform/mk66/usb/command_table.h,app/src/module_ui.cpp,platform/mk66/src/app_main.cpp spec=docs/specs/OS-INSFX.md qa=QA-GLB-161,QA-GLB-162,QA-GLB-163,QA-GLB-164,QA-GLB-165,QA-MIX-072 -->

The console has more commands than this table. The table lists the ones that are useful to a player or a tester. Each line is the command as you type it.

| Command | What it shows or does | Notes |
|---|---|---|
| `help` | lists the commands | |
| `version` | the build and the USB identity | the same build as Config, INFO |
| `cpu` | the audio load: cycles per stage, underruns, jitter, and a `guard` line: how many audio blocks the player skipped (shed) because the screen and USB handling was starved by a very heavy project (normally `shed 0 blocks`) | `cpu reset` sets the peaks and fault counters back to zero, so you can measure one run, and starts the module timing again. `cpu perf off` stops that module timing. |
| `guard` | the `guard` line of `cpu` on its own, with `n`, the window: how many heavy blocks in a row the player renders before it skips blocks to give the screen and USB handling their time | `guard n 12` sets the window (1 to 255, default 64) for a test run; it is not saved, a restart goes back to 64 |
| `mem` | the memory map and the free SRAM | |
| `crash` | the crash record that the last run left | |
| `usb` | the USB counters and the AUDIO mode | also `usb audio`, `usb mode`, `usb profile` and `usb map` |
| `log` | the last lines that the boot code and the drivers wrote | `log 30` shows the last 30 lines |
| `mix` | the audio meters in dBFS and the limiter gain reduction | |
| `mon` | the line-in monitor level | `mon off`, `mon on` (level 70), or `mon 0` to `mon 100` |
| `sd` | the SD card: slot, mount, identity, speed and counters | also `sd mount` |
| `ls` | a folder of the card, read only | for example `ls Projects` |
| `fw` | the firmware files on the card, and the card's volume serial | also `fw put`, `fw rm`, `fw arm` and `fw update`. Use the tool for these, see below |
| `radio` | the FM radio state, its last failure, its registers and pins | `radio retry` tries again |
| `ins` | the inserts of the project: the totals of the insert stage, then one line for each bound insert with its track, slot, module, `on` or `bypassed`, and its cost | `ins bind <track> <slot> <module>` puts an effect on a track and loads the module from the card when it is not loaded yet; `ins unbind <track> <slot>` removes it; `ins bypass <track> <slot> on` or `off` bypasses it. For the master insert, which runs its effect twice (left and right) and so lists two lines, `master L` and `master R`, type `master` instead of the track and slot: `ins bind master <module>`, `ins unbind master`, `ins bypass master on`. A refusal says why and changes nothing: **DELETE THE INSERT FIRST** (the slot is taken), **NO INSERT HERE** (the slot is empty), **NOT AN EFFECT MODULE**, **MODULE MISSING**, **INSERTS FULL**. These are the INSERTS page's messages, see [Mixer](mixer.md) <!-- QA-GLB-161, QA-GLB-162, QA-GLB-163, QA-GLB-164, QA-GLB-165, QA-MIX-072 --> |
| `voices` | the sampler voices in use, the peak since boot, the steals and `advanced` | `advanced` counts the short stretches (one a block, about 690 a second for each) in which a voice on a muted track was moved along without being rendered. It stays at 0 while no track is muted. |
| `rot` | the display orientation, `rot 0` to `rot 7` | not saved |
| `status` | the status LED mode, with a count of beat flashes: `status blink`, `status bpm` or `status off` | for this session only, not saved: Config > DISPLAY > STATUS LED is the saved setting and every boot starts with that; with off the over-budget code 12 stays dark too, a fault code still blinks (new Community OS behaviour) |
| `state` | the app state as one `key=value` line | for example the screen, the cursor and play |
| `shot` | the screen, as framed data | the tool `--shot` option asks for it |
| `leds` | the pad LED levels, on one line | |
| `key` | presses a button: `key <button> [tap, down, up or hold <ms>]` | for testing. The unit reacts as to a real press |
| `pad` | presses a pad, `pad 0` to `pad 47` | for testing |
| `enc` | turns the encoder, `enc -64` to `enc 64` detents | for testing; a minus is left. `enc accel` shows how fast a quick spin moves a list (new Community OS behaviour: in the File and Sample load lists, the Modules list and the module picker a fast spin moves 4 or 8 rows a detent); `enc accel off` goes back to a row a detent, `enc accel 6 16 60` sets x4 after 6 detents, x8 after 16, and a run ends after 60 ms without a detent. Not saved: a restart goes back to 6 16 60 |
| `reset` | mutes the outputs and restarts the unit | |

For developers only, and not in the table: `peek` reads 32-bit words, `poke` writes one word, `bench` is for the bench image, and `stage`, `audio`, `load`, `inst`, `mod`, `input` and `cache` are for debugging.

### Take a screenshot
<!-- sync: src=platform/mk66/tools/o3console.py,platform/mk66/usb/src/shot_stream.cpp -->

The tool asks the unit for its screen, checks the picture and writes a PNG file:

```
python3 -I platform/mk66/tools/o3console.py --shot out.png
```

Commands given after the option run first, in order. This presses `F1` and then takes the picture:

```
python3 -I platform/mk66/tools/o3console.py --shot out.png "key f1"
```

A picture that arrives damaged, or that changed while it was sent, is asked for again. The tool tries again up to 2 times. Its exit status is `0` when the file is written, `2` when there was no prompt or the transfer stalled, and `4` when the unit refused the request or the data was not a whole picture.

The unit can also save its own screen to the card with no computer. This is new in the Community OS. Hold **Shift**, press **Power**, then let go of both. The picture is the first screen drawn after you let go, so the key row shows its normal labels. It is written to `Screenshots/SHOT_0001.bmp` (the next is `SHOT_0002.bmp`, and a file is never overwritten). The screen freezes for a second or two while it is written and the sound goes on; the strip then says `SCREENSHOT SAVED: SHOT_0001.BMP`. With no card, the card on the computer, or a save or export running, the strip says `NO SCREENSHOT:` and the reason.

### Send a firmware file to the card
<!-- sync: src=platform/mk66/tools/o3console.py,platform/mk66/update/include/o3os/platform/mk66/update/path_guard.h -->

Use `--put` to copy a `.ptf` file into the `Firmware/` folder of the card. The tool checks the file first.

```
python3 -I platform/mk66/tools/o3console.py --put track3r-os-<version>.ptf
```

1. Any SD card in the unit takes the file. To make the tool check which card it is, find the card's volume serial (the `fw` command prints it in the form `XXXX-XXXX`) and pass it with `--expect-volume XXXX-XXXX`. The tool then refuses to write to a different card.
2. The name on the card must start with a letter or a digit. It may use letters, digits, dots, underscores and dashes, and it must end in `.ptf`. The whole name, `.ptf` included, has at most 30 characters: with a longer name the SD bootloader was seen flashing the first `.ptf` of `Firmware/` instead of your file, so `--put` and `--update` refuse it before anything is written. Use `--name` to give the file another name on the card.
3. The unit checks the file again after it is written. The tool prints `ok`, the name, the size, the CRC-32 and the image length.
4. An older or foreign image that only lacks its padding (it must end on a 4 KiB boundary: v1.0.0 does not) is refused by `--put`. Add `--pad` and the tool writes a padded copy on your computer, checks the whole copy and sends it as `pad-<start of the name>.ptf`; the file on your computer is not changed. Any other fault, a wrong flash configuration byte included, is still refused. A file that already passes is sent as it is. New Community OS behaviour.

A file on the card does not update the unit. To flash it, follow the update steps in [Installing track3r-os](install.md). The unit flashes the first `.ptf` in `Firmware/`, so keep only one.

## Measure the audio: o3audio.py
<!-- sync: src=platform/mk66/tools/o3audio.py -->

The unit shows its audio device to the computer when USB AUDIO is on. This tool records that device for a few seconds, then reports on each channel. It needs `arecord` from the alsa-utils package.

1. List the capture devices and see which one looks like the unit:

   ```
   python3 -I platform/mk66/tools/o3audio.py list
   ```

2. Record 3 seconds and save the recording, if you want to keep it:

   ```
   python3 -I platform/mk66/tools/o3audio.py record --seconds 3 --save out.wav
   ```

3. Or analyse a file you already have: `python3 -I platform/mk66/tools/o3audio.py analyze out.wav`.

For each channel the tool reports the peak and the RMS level in dBFS, the DC offset, whether the channel is silent, whether it clips, and the strongest pitch when there is one. Checks can make the tool fail on purpose, for example `--expect-pitch 440` (with `--cents 40`), `--not-silent`, `--max-peak -1` or `--no-clipping`. `--channel 0` checks every channel. The tool exits with `0` when every check passed, `1` when a check failed, and `2` when there was no capture.

## Take a screen from a saved text: o3shot.py
<!-- sync: src=platform/mk66/tools/o3shot.py -->

Most people do not need this. `o3shot.py` works on the same screen data as `--shot`. It has two uses:

- `decode` turns a text file of a `shot` reply, saved from a terminal, into a PNG: `python3 -I platform/mk66/tools/o3shot.py decode capture.txt out.png`.
- `compare` checks two 800 by 480 PNG files, with tolerances for the small differences between a real screen and the simulator, and writes an optional difference picture with `--diff`. It exits with `0` when the pictures agree.

## See also

- [Installing track3r-os](install.md): the update steps, the checks and the way back.
- [Getting started](getting_started.md): the card, the first start and the firmware file.
- [The panel](panel.md): the keys that the `key` command presses.
- [Glossary](glossary.md): the terms of this chapter.
