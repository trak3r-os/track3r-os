<!-- manual-sync: 32c3e0c -->
# Installing track3r-os

Status: draft

The release file is an Intel HEX image under a `.ptf` name, which is what the Tracker's bootloader flashes, and the bootloader itself is not touched by an update. Once track3r-os runs, later updates can also be sent from a computer with `o3console.py`, and the official firmware can be put back. This page says what has been checked and marks the rest **not yet verified**. <!-- og: open -->

> **Unofficial, at your own risk.** track3r-os is community firmware. It is not affiliated with or endorsed by Polyend. Installing it replaces the firmware in your unit. The bootloader that flashes it is separate from the firmware and is not touched by an update (`docs/hw/tracker-hardware-notes.md`, section 3), but nothing here is a guarantee. Read the whole page first.

This page says what is established by observation on the maintainer's unit and by the repository's own tests, and marks the rest **not yet verified**. Where a step is not verified, do it only if you accept that risk and have a way back (see [Going back](#going-back-to-the-official-firmware)).

## What a release contains
<!-- sync: src=tools/release/make_starter.py,tools/release/starter_manifest.json -->

A release (GitHub, "Releases") has two files:

- `track3r-os-<version>.ptf`: the update file. It is the device image in Intel HEX form under a `.ptf` name, which is what the Tracker's bootloader flashes. Its name never starts with `Polyend`, so it cannot be mistaken for an official file. For a first install through the reset button you rename your copy on the card (section 1).
- `SHA256SUMS`: the checksum of the update file. Check the download before using it: `sha256sum -c SHA256SUMS`. The release notes also give the CRC-32 that `o3console.py --update --crc` asks for.

The optional starter content (samples, wavetables, songs, starter projects, the bundled apps, licences) is a separate zip, `track3r-os-<version>.zip`. It is not part of this release. When you have it, unpack it into the root of the card. Its `README.txt` says what goes where.

The same image shows its version in Config, INFO (BUILD ID, for example `v0.5.0 (0271ab60bea6)`), on the boot screen, and in the `version` command of the USB `diag` console.

## Before you start
<!-- sync: src=platform/mk66/update/include/o3os/platform/mk66/update/path_guard.h,docs/hw/tracker-hardware-notes.md -->

1. **Back up the SD card.** Copy everything on it to your computer: projects, samples, modules and the `Firmware/` folder (it holds the official image you may want to go back to). The USB update path writes only inside `Firmware/` (`platform/mk66/update/include/o3os/platform/mk66/update/path_guard.h`), but the card is what the bootloader reads, and a card is easy to damage when it is pulled and reinserted.
2. **Have the official firmware file at hand.** Official images are named `PolyendTracker_<version>.ptf`. Get one from Polyend; their pages are linked from `docs/hw/tracker-hardware-notes.md` ([how to update](https://polyend.ladesk.com/220365-How-do-I-update-Trackers-firmware), [emergency flashing](https://polyend.ladesk.com/929798-How-do-I-perform-the-emergency-flashing-procedure)). The vendor's emergency-flashing page is also where the hidden reset button used below is described.
3. **Use a normal card.** The Tracker needs a single FAT32 partition with an MBR. The track3r-os writer refuses exFAT volumes and cards with a write-protected CSD, and it never creates the `Firmware/` folder: it must already exist.
4. **Settings.** The vendor tells users to create a new project after an update so that Config is initialised (the vendor's statement, not verified here). Config may start from defaults after a change of firmware in either direction. How the settings of track3r-os and the official firmware interact is **not yet verified**.
5. **Power.** Use a stable USB power source and do not remove power or the card while the unit is updating. This is a precaution; how long the flashing takes and what the screen shows while it runs has not been recorded.

## 1. From the official firmware
<!-- sync: src=platform/mk66/README.md,docs/hw/tracker-hardware-notes.md -->

What is established:

- The bootloader flashes a `.ptf` (Intel HEX) from `Firmware/` on the card. It does flash a file whose name does not start with `PolyendTracker_` (observed on the unit, 2026-10-12, with a track3r-os test build started from the running track3r-os; `platform/mk66/README.md`, "Firmware update over USB").
- The official firmware's update menu has a file-name filter. Official 1.9.0 lists only names starting with `PolyendTracker` (observed on the unit; the filter of other versions is not known). **A track3r-os file named `track3r-os-<version>.ptf` is therefore not shown in the official update menu.** The release file is deliberately named differently, so that it cannot be mistaken for an official file. For the first install the copy on the card is renamed to start with `PolyendTracker` (see the steps below). The name changes two things. The official update menu lists the file. And track3r-os treats every name starting with `PolyendTracker` as official, so `fw rm` does not remove it: delete it from the computer if you want it gone.
- The hidden reset button flashes the **first** `.ptf` in `Firmware/` in directory order. This is how the maintainer's unit was recovered from every hung test image. The images it flashed there were named `PolyendTracker_0.0.x.ptf` (24 characters).
- The Polyend Tool app is **not supported** for this. It accepts only official files and answered "invalid firmware" to a track3r-os file. Two units were reported stuck on the "emergency update" screen with the file `track3r-os-v1.0.0.ptf`. Install straight from the SD card, as below.
- Users' reports (2026-10-25): the release file renamed to `PolyendTracker_0.0.1.ptf` installed from the card on a unit that ran the official firmware, after the same file under its release name had stuck on the "emergency update" screen. Another user reported that with the renamed file the reset button is not needed: the official firmware's own update menu installs it. Neither has been repeated on the maintainer's unit, and the users' models are not recorded.

The steps this leaves:

1. Back up the card (above).
2. Copy `track3r-os-<version>.ptf` into the `Firmware/` folder of the card, and rename the copy to `PolyendTracker_0.0.1.ptf`. The new name must start with `PolyendTracker` and be at most 24 characters; `PolyendTracker_0.0.1.ptf` is exactly 24. Do not change the content of the file. Do not use the Polyend Tool app.
3. Make it the **only** `.ptf` in `Firmware/`: move any other `.ptf` (including the official one) out to your computer. With several files the button picks the first in directory order, which you cannot easily predict.
4. Put the card in the unit and use the hidden reset button as the vendor's emergency-flashing page describes. The position of the button is not recorded in this repository. A user reports that a unit running the official firmware also installs the renamed file from its own update menu, without the button (not checked here).
5. When the unit has restarted, open Config, INFO and check that BUILD ID shows the version you installed (the file name does not say what is installed; BUILD ID does). If the display looks corrupted on the first boot, power-cycle the unit: this was seen once after the bootloader flashed an early test image and a power cycle fixed it.

**Reported working, not yet repeated here:** steps 2 to 4 with the release file renamed to `PolyendTracker_0.0.1.ptf`, on a unit that ran the official firmware (a user's report, 2026-10-25). On the maintainer's unit the reset button was observed flashing `PolyendTracker_0.0.x.ptf` images (24 characters); a name not starting with `PolyendTracker` was flashed there only through the USB update path from a running track3r-os. If the file does not flash, nothing is changed on the unit; say so in an issue with the file name you used and your model.

Later updates from a running track3r-os (section 2, or Config, INFO, `F8`) keep the plain `track3r-os-<version>.ptf` name.

## 2. Updating track3r-os to a newer track3r-os
<!-- sync: src=platform/mk66/tools/o3console.py spec=docs/specs/OS-006-usb.md -->

An image of track3r-os has the `diag` console on its USB serial port and can write a new image to the card and ask the bootloader to flash it (OS-006c, `docs/specs/OS-006-usb.md`). The tool is `platform/mk66/tools/o3console.py` (Python 3, standard library only; it needs `check_image.py` from the same folder, so use a checkout of this repository). It checks the file locally and refuses a file that fails.

1. Back up the card. Connect the unit by USB (it enumerates as `16d0:0e9f`, the same USB ID as the official Tracker; the product name is "Track3r", the maker "track3r-os").
2. Any SD card in the unit takes the file. To make the tool check the card, find its volume serial (the `fw` command of the `diag` console prints it as `XXXX-XXXX`, a FAT volume ID) and pass it with `--expect-volume XXXX-XXXX`; the tool then refuses to write to a different card.
3. Send the file. The name must match `[0-9A-Za-z][0-9A-Za-z._-]{0,47}.ptf`:

   ```sh
   python3 -I platform/mk66/tools/o3console.py --put track3r-os-v0.5.1.ptf --name track3r-os-v0.5.1.ptf
   ```

   The unit validates the stream while it writes, reads the file back from the card and checks it again, and answers `ok <name> <bytes> <crc> image <length>`. It never leaves a half-written `.ptf`.
4. Update. This restarts the unit into the bootloader, so the tool asks for `--yes-update`. `<crc>` is the CRC-32 from the release notes (the `ok` line of the put prints the same value):

   ```sh
   python3 -I platform/mk66/tools/o3console.py --update track3r-os-v0.5.1.ptf --crc <crc> --yes-update --expect-version v0.5.1
   ```

   The tool waits for the unit to come back, runs `version`, and fails if the version does not contain the text you expect. If the bootloader does not take over within 5 seconds the unit reports `ERR bootloader did not take over` and the update is cancelled.
5. Afterwards `Firmware/` holds every `.ptf` you put there. Remember the reset-button rule: it flashes the first one. `fw` marks that file with `*`. Remove the files you do not want (`fw rm <name>`; it keeps the last `.ptf`, and never removes a name starting with `Polyend`), or move them off the card on your computer.

What has been seen: the update path flashed a track3r-os test build on the maintainer's unit and the unit came back as that build (2026-10-12). **Not yet verified:** the whole sequence above with a release file, and several of the checklist items in `platform/mk66/README.md` (FW-3 to FW-10, for example a card with its write-protect tab set, a pulled cable during the upload, the fallback after five seconds).

The alternative is the card route of section 1: put the new file alone in `Firmware/` and use the reset button.

A unit that already runs a track3r-os with the update dialog can also install a `.ptf` that is on its card from the unit itself, with no computer: see [Update from the unit](config.md#update-from-the-unit) in the Config chapter.

## Going back to the official firmware

The bootloader is separate from the application, so a broken or unwanted application does not remove the recovery path. The way back that has been proven on the unit is the hidden reset button with a single good `.ptf` in `Firmware/`.

1. Put the card on your computer. Move **every** `.ptf` out of `Firmware/` (the track3r-os files and any other).
2. Copy the official `PolyendTracker_<version>.ptf` you kept (or downloaded from Polyend) into `Firmware/`, so that it is the only `.ptf` there.
3. Put the card in the unit and use the hidden reset button as in the vendor's emergency-flashing page.
4. When the official firmware is running, create a new project if its Config looks reset, as the vendor advises after an update.

track3r-os has no firmware update menu on the device: updates come from the computer or by card. The official firmware's own update menu will list an official file named `PolyendTracker...`, but that route has **not yet been verified** from track3r-os as the starting point.

**Not yet verified:** that the official firmware opens projects saved by track3r-os (it reads the original formats; the native track3r-os project format is its own), and what happens to the unit's stored settings when you switch back. Keep your original-firmware projects as they are and work on copies.

## If something goes wrong

- The unit does not start after an update: do the steps of [Going back](#going-back-to-the-official-firmware). It is the same recovery used for every hung test image.
- The reset button flashed a file you did not expect: there was more than one `.ptf` in `Firmware/`. Leave exactly one.
- The unit sits on the "emergency update" screen and does not flash: check that the file on the card is named `PolyendTracker_0.0.1.ptf` (not `track3r-os-<version>.ptf`) and that it is the only `.ptf` in `Firmware/`. Do not use the Polyend Tool app.
- The pads do not answer after the install, while the buttons and the screen work: this happened with v1.0.0 on later units (plastic back; issue #1) and is fixed in v1.0.1, which a user with such a unit confirmed (2026-10-26). If your unit runs v1.0.0, install v1.0.1 the same way. If the pads still do not answer on v1.0.1, report it (OS-BUG-PADS). Open the [Input test](input_test.md) screen (hold `Shift`, `F1` and `F8` while you switch on) and press some pads. The board's status LED (a user saw it blinking behind the buttons under the screen) gives one short blink a second while track3r-os runs; 13 quick flashes after power-on, before that blink starts, mean a key scanner did not answer at boot. Report both in an issue with your model, and go back to the official firmware if you need the pads ([Going back](#going-back-to-the-official-firmware)).
- The unit is not found by `o3console.py`: check the USB cable, and that the unit runs track3r-os (the `diag` console is part of track3r-os).
- Report problems with the version string from Config, INFO (or the `version` command) and the file name and SHA-256 you installed.

## Not yet verified (summary)

| Item | Status |
|---|---|
| The release file renamed to `PolyendTracker_0.0.1.ptf` installs on a unit that runs the official firmware | reported working by a user (2026-10-25); method and model not recorded; not repeated on the maintainer's unit |
| The official firmware's update menu installs the renamed file, without the reset button | reported by a user (2026-10-25); not checked here |
| The pads on every board revision | v1.0.0 left the pads dead on a later unit (plastic back, 2026-10-25, OS-BUG-PADS); v1.0.1 works there (one user, 2026-10-26); other revisions not reported |
| Position of the hidden reset button | not recorded here; see the vendor page |
| Time the flashing takes and what the screen shows | not recorded |
| Full USB update sequence with a release file, and checklist FW-3 to FW-10 | not yet verified |
| Official firmware's update menu as a way back from track3r-os | not yet verified |
| Settings and project compatibility between track3r-os and the official firmware | not yet verified |
