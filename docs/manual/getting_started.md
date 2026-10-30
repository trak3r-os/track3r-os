<!-- manual-sync: 48ab84b9 -->
<!--
Chapter: Getting started. Rules: .claude/skills/manual-writing/SKILL.md. Scope: what the OS is, what goes on the card, the first start, the way back. The install steps are in install.md and are linked, not repeated.
Sources: docs/manual/install.md (release files, before you start, update path, going back, checks), README.md (what it is, files and compatibility, get started), docs/specs/OS-007-platform.md (project load at boot, demo kit), docs/manual/file.md (projects, USB disk mode), docs/manual/panel.md (keys), docs/qa/catalogue/global.md (QA-GLB-015 to 017, 019, 021, 023: the disk mode steps; QA-GLB-100 to 117: the Power off), docs/roadmap/BACKLOG.md (OS-STARTER, queued). Not written from the GLB rows: QA-GLB-020 (the copy test has not been run yet) and QA-GLB-022 (the bench image's diag command, not for players).
Manifest lines needed: none (docs/manual/images.txt does not exist yet; this chapter has no picture).
Open rows, not written: none.
-->
# Getting started

Status: draft

track3r-os runs in place of the official firmware on the same unit and the same card, and the way back to the official firmware stays open. Read this chapter once before you install anything: it says what goes on the card, what the first start looks like and how to return. The install steps themselves are in [Installing track3r-os](install.md). For your first sound, go to [Quick start](quick_start.md).

## What it is for

track3r-os is an operating system for the Polyend Tracker. The community wrote it. It is unofficial and not affiliated with or endorsed by Polyend. It runs in place of the official firmware on your unit. Read this chapter once before you install anything.

> **Careful**: track3r-os is unofficial community firmware, and some steps of the install and the way back have not been run on a unit with the release file (the release notes list them). Back up the whole SD card before you install anything. Copy everything on it to your computer: projects, samples, modules and the `Firmware/` folder.

## Before you start

You need:

- a Tracker, with a USB cable;
- an SD card with one FAT32 partition. track3r-os refuses exFAT cards;
- a computer, to copy files and to download the release.

Keep a copy of the official firmware file on your computer, named `PolyendTracker_<version>.ptf`. You need it to go back. The vendor pages are linked from [Installing track3r-os](install.md).

## What goes on the card
<!-- sync: src=tools/release/make_starter.py -->

The card holds your projects and the files the unit reads at start-up.

| Folder or file | What it holds |
|---|---|
| `Firmware/` | the firmware file, `.ptf`. The folder must already exist. track3r-os does not create it. |
| `Workspace/` | the project the unit loads at boot when no recently opened project is on the card, if the folder holds `project.mt`. The project is in the original firmware format. |
| `Projects/` | the projects you save, one folder for each. |
| `modules/` | the `.tmod` files: instrument and effect modules, file importers and apps. Apps go in `modules/apps/`. |

A starter zip is not part of this release. If you have one, you unpack it into the root of the card. It is meant to hold these folders and files:

| In the zip | What it holds |
|---|---|
| `Firmware/` | the update file and its checksum |
| `modules/apps/` | apps for the TOOLS screen. The TB-303, chip, FM and SID instruments are inside the firmware. The apps alone come in `track3r-os-<version>-apps.zip`, in the release `apps-v<version>`. |
| `Samples/` | a drum kit and other one-shots |
| `WTABLE/` | wavetables |
| `Songs/` | tracker songs for the importers |
| `Projects/` | two demo projects |
| `LICENSES/` | the licence texts and the credits |
| `README.txt` | what goes where |
| `SHA256SUMS` | the checksum of every file |

## More tasks

### Install track3r-os

Follow [Installing track3r-os](install.md). It has the update steps, the checks and what to do when the unit does not start. This chapter does not repeat them.

1. Download the release from the project's Releases page on GitHub. It has two files: `track3r-os-<version>.ptf` and `SHA256SUMS`. The starter zip described above is not part of the release. The apps are a separate download: the release `apps-v<version>` on the Releases page holds `track3r-os-<version>-apps.zip`. Unpack it into the root of the card.
2. Check the download with `sha256sum -c SHA256SUMS`.
3. Back up the card, then follow the steps in [Installing track3r-os](install.md).

### Check the version

Use this after an install, to confirm which build the unit runs.

1. Press `Config`.
2. Open the INFO page.
3. Look at **BUILD ID**. It shows the version and a short build code, for example `v0.5.0 (0271ab60bea6)`.

You should see the version you installed. The boot screen shows the same version.

### Start the unit for the first time
<!-- sync: src=ui/include/o3os/ui/boot_screen.h,platform/mk66/src/app_main.cpp -->

1. Turn the unit on.
2. The boot screen shows a logo with **Track3r** and the build under it. It stays for at least 0.8 s, until the start-up work is done.
3. The unit opens the project you had open last (the newest of the File screen's RECENT list) if it is still on the card. With none, and a `Workspace/project.mt` file on the card, it loads that. When the last project cannot be read (a damaged file, a project from a newer OS), the unit starts empty and the strip says `PROJECT <name> FAILED` and why; it does not fall back to the Workspace, and it tries the same project on every start until you open another one in File. The strip shows the end of the load: `PROJECT <name> OPENED`.
4. If no project loads, or the loaded project has no sample or module instrument and no instrument at all in slots 1 to 48 (MIDI instruments in slots 49 to 64, the 16 MIDI channels of every project from the official firmware, do not stop it), the unit starts the demo kit: four instruments and pattern 1. This is new Community OS behaviour, a default the maintainer still has to confirm. Press `Play` to start pattern 1.

To open another project from the card, press `File`. See [File](file.md). For where each key is, see [The panel](panel.md). On any screen, hold `Shift` and tap `Config` to read a help page for it ([the help page](panel.md#the-help-page)). <!-- QA-GLB-141 -->

### Turn the unit off

`Power` is the small button on the rear panel. Save your work first: press `F4` (**SAVE**) on the File screen. The off does not save for you.

![Panel: the Power button on the rear panel is ringed. Hold it for 2 seconds.](img/chord_power_hold.svg)

1. Hold `Power`. A line across the top of the screen fills over 2 seconds. <!-- QA-GLB-100 -->
2. Keep holding until the line is full. The strip says **POWERING OFF**. The keys and pads stop working, the screen fades, the pad lights go out and the sound stops. <!-- QA-GLB-102 -->
3. Let go of `Power`. The unit stays dark. Press `Power` and let go to start it again. <!-- QA-GLB-102, QA-GLB-103, QA-GLB-114 -->

To cancel, let go before the line is full, or press `Shift` while you hold `Power`. <!-- QA-GLB-100, QA-GLB-110 --> `Shift` + `Power` on its own is not an off: it saves a screenshot to the card ([The panel](panel.md#screenshot)). <!-- QA-GLB-130 --> To remove all power, unplug the USB cable. For saves and exports during the off, see [The panel](panel.md#power). **Checked on a unit (test build v0.6.12):** the off, the dark screen, and a press of `Power` to start again. **Known gap:** a click may be heard at power off and at power on. **Not yet checked on the unit:** cancelling with `Shift`, saves and exports during an off, and the current draw while the unit is off.

### Use the card on a computer

In this mode the unit shows its card to the computer as a disk. The unit does not load a project and does not open its audio outputs. Start with the unit off and connected to the computer with USB. <!-- QA-GLB-015 -->

1. Hold `Shift`, `F1` and `F7` together, and turn the unit on (the keys are on the figure in [The panel](panel.md#keys-held-when-the-unit-turns-on)). Keep them held until the strip says **USB DISK - EJECT ON THE COMPUTER OR PRESS A KEY**. <!-- QA-GLB-015, QA-GLB-016 -->
2. Let the keys go. The computer lists the card as one writable disk. <!-- QA-GLB-015, QA-GLB-019 -->
3. You can unmount and mount the card on the computer without ejecting it. The card comes back with the same files. <!-- QA-GLB-021 -->
4. When you are done, eject the disk on the computer. Then press a key on the unit. If the computer has not ejected the disk, press a key twice within 5 seconds: the first only warns. <!-- QA-GLB-017, QA-GLB-023 -->

The strip says **DISK EJECTED - PRESS ANY KEY TO RESTART**, and the unit restarts with the card in place. <!-- QA-GLB-017 --> Checked on a unit (test build v0.6.10); tested on Linux only. For the details, and for sharing the card while the unit plays, see [USB disk](usb_disk.md).

### Go back to the official firmware

Follow [Going back to the official firmware](install.md#going-back-to-the-official-firmware). It has the steps and the checks.

Keep your projects from the original firmware as they are. Work on copies in track3r-os.

## Good to know
<!-- sync: src=docs/formats/og.md -->

- Projects saved by the original firmware open in track3r-os. This is tested on the host with files made from the format's byte maps. Not yet checked on the unit.
- Projects saved in track3r-os may not open on the official firmware. **Not yet verified.**
- The unit's settings may start from defaults after a change of firmware, in either direction. **Not yet verified.** Keep copies of your projects.

## See also

- [Installing track3r-os](install.md): the update steps, the checks and the way back.
- [The panel](panel.md): where each key is and what it does.
- [File](file.md): projects, saving and loading.
- [USB disk](usb_disk.md): the card on a computer.
- [Computer tools](computer_tools.md): the USB tools for a computer.
