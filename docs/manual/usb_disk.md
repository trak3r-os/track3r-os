<!-- manual-sync: 48ab84b9 -->
<!--
Chapter: USB disk. Rules: .claude/skills/manual-writing/SKILL.md. Moved out of file.md (OS-MANUAL2 C4 fold): the disk material of that chapter, with its facts. Medium tier.
Sources: docs/qa/catalogue/global.md (QA-GLB-015, 016, 017, 019, 020, 021, 023), docs/qa/catalogue/config.md (QA-CFG-200 to 210), docs/specs/OS-USBDISK.md (sections 6 and 7: the write cache, the power cut), docs/STATUS.md (unit run of the boot disk on v0.6.10).
Try it: no walk scenario. The simulator has no USB host, so the steps are the QA rows and the strip texts are not pictured.
Open rows, not written: none.
-->
# USB disk

Status: draft

The card can go to a computer as a disk. There are two ways. Hold `Shift`, `F1` and `F7` while you turn the unit on, and the unit does nothing but show its card. Or use the **CARD AS DISK** row in `Config`, and the unit keeps playing while the computer has the card. New in track3r-os. <!-- og: new -->

## What it is for

Use it to copy samples, modules and projects to and from the card without taking it out of the unit. Use the power-on way when you only want the files. Use the `Config` way when you want to copy while a pattern plays, or while you record and mix.

## How it works
<!-- sync: spec=docs/specs/OS-USBDISK.md qa=QA-GLB-015,QA-CFG-200,QA-CFG-201,QA-CFG-202 -->

A card has one owner at a time: the unit or the computer. While the computer owns it, the unit never touches it. This is why the two ways differ in what the unit still does. <!-- QA-GLB-015, QA-CFG-202 -->

- **At power-on**, the unit loads no project and opens no audio outputs. It is a card reader until you eject.
- **From `Config`**, everything already in memory goes on: your pattern plays, and you can record, edit, mix and use the pads and MIDI. What stops is everything that reads or writes the card: saving, loading, the file and sample lists, exports and modules from the card. They say **CARD ON COMPUTER**. <!-- QA-CFG-200, QA-CFG-201 -->

The card is live on the computer, and nothing of it is stored in the project. The share ends when you eject.

An example: start a loop, give the card to the computer and drop a new sample on it, then eject. The loop never stopped, and the sample is in the list when the card is back.

## Try it
<!-- sync: spec=docs/specs/OS-USBDISK.md qa=QA-CFG-200,QA-CFG-203,QA-CFG-210 -->

Start with a pattern playing. Connect the unit to the computer with USB.

1. Open `Config`, press `F5` (**USB**) and move down to **CARD AS DISK**. <!-- QA-CFG-200 -->
2. Press `F7`. The strip asks **THE COMPUTER GETS THE CARD: SAVES STOP. F7 AGAIN**. <!-- QA-CFG-200 -->
3. Press `F7` again. After a few seconds the computer lists the card, and the strip says **CARD ON COMPUTER - CONFIG USB: F7 EJECT**. The pattern keeps playing. <!-- QA-CFG-203 -->
4. On the computer, copy a file to the card, then eject the disk. <!-- QA-CFG-203 -->
5. The strip says **READING CARD** and then **CARD READY**. The card is the unit's again. <!-- QA-CFG-203 -->

The computer could only read the card in this walk, because **READ ONLY** starts **On**. To copy to the card, set it to **Off** before step 2 (see below).

## More tasks

### Use the card on a computer at power-on
<!-- sync: spec=docs/specs/OS-USBDISK.md qa=QA-GLB-015,QA-GLB-016,QA-GLB-019,QA-GLB-017,QA-GLB-023 -->

Start with the unit off and connected to the computer with USB.

1. Hold `Shift`, `F1` and `F7` together while you turn the unit on, until the strip shows **USB DISK - EJECT ON THE COMPUTER OR PRESS A KEY**. Do not hold `F8` as well: with `F8` down the **INPUT TEST** screen opens instead. <!-- QA-GLB-015, QA-GLB-016 -->
2. Let go. The computer lists the card as a disk it can read and write. <!-- QA-GLB-019 -->
3. When you are done, eject the disk on the computer, then press a key on the unit. <!-- QA-GLB-017 -->

The strip says **DISK EJECTED - PRESS ANY KEY TO RESTART**, and the unit restarts with the card in place. If the computer has not ejected the disk, the first key only warns: **THE COMPUTER MAY STILL BE WRITING: KEY AGAIN**. Press a key again within 5 seconds to restart. A restart during a copy waits up to 3 seconds for the write in progress (**FINISHING THE LAST WRITE - RESTARTING**). <!-- QA-GLB-017, QA-GLB-023 -->

> **Careful**: eject the disk on the computer before you unplug the cable. The computer may hold writes it has not sent yet, and a restart in the middle of a copy can leave the computer's file system to repair. <!-- docs/specs/OS-USBDISK.md, write cache section; QA-GLB-023 -->

### Let the computer write to the card while the unit plays
<!-- sync: spec=docs/specs/OS-USBDISK.md qa=QA-CFG-200,QA-CFG-210 -->

Start on `Config`, on the USB page (`F5`).

1. Move down to **READ ONLY**. **On** (the start value) lets the computer read the card only. **Off** lets it write too. <!-- QA-CFG-200 -->
2. Turn the encoder to **Off**. The choice stays in memory until the unit restarts, is not saved with anything, and holds for several shares in a row. <!-- QA-CFG-200, QA-CFG-210 -->
3. Go back to **CARD AS DISK** and share the card as in Try it.

Once you have shared the card, **READ ONLY** is fixed until you eject: the strip says **EJECT THE CARD TO CHANGE IT**. While the card changes hands it says **THE CARD IS CHANGING HANDS: WAIT**. <!-- QA-CFG-210 -->

### Get the card back from the computer
<!-- sync: spec=docs/specs/OS-USBDISK.md qa=QA-CFG-203,QA-CFG-204,QA-CFG-208,QA-CFG-209 -->

Eject the disk on the computer, or press `F7` twice on the **CARD AS DISK** row. The unit warns first (**THE COMPUTER MAY STILL BE WRITING: F7 AGAIN**), because a copy still running is cut. The strip then says **READING CARD** and **CARD READY**. A card marked as not closed properly gives **CARD NOT EJECTED PROPERLY: CHECK THE COMPUTER**, and everything still works. An exFAT card shows **EXFAT CARD: READ ONLY**: the unit can read it but not save to it. The unit never lists names that start with a dot or the **System Volume Information** folder. <!-- QA-CFG-203, QA-CFG-204, QA-CFG-209 -->

> **Careful**: eject on the computer before you unplug the cable. If you unplug without ejecting, the card stays with the computer until you take it back with `F7` twice. <!-- QA-CFG-208, QA-CFG-209 -->

### See what the unit refuses while the card is out
<!-- sync: spec=docs/specs/OS-USBDISK.md qa=QA-CFG-200,QA-CFG-201,QA-CFG-205,QA-CFG-207 -->

- Saving, loading, the file and sample lists, exports and modules from the card say **CARD ON COMPUTER**, and so do File `F1` (OPEN), `Delete` and `F7` (EXPORT), Sample load `F4` (ADD) and `F5` (PLAY) and the instrument LOAD. The refusal comes up on the strip at once. <!-- QA-CFG-211 --> A change to your settings is kept in memory and written when you get the card back. Recording works, but saving the take as a WAV file waits. <!-- QA-CFG-201, QA-CFG-205 -->
- **NO CARD** or **STOP THE EXPORT FIRST** at the first `F7` means nothing was started. **CARD BUSY: WAIT FOR THE SAVE** means a save was still running. <!-- QA-CFG-201 -->
- In USB AUDIO mode the strip says **USB AUDIO PAUSES** until you eject. <!-- QA-CFG-207 -->

> **Careful**: settings you change while the card is out are only in memory. If you switch the unit off before you get the card back, they are lost. <!-- docs/specs/OS-USBDISK.md section 7: a power cut while the computer has the card -->

## Good to know

- Tested on Linux only. Windows and macOS are untested.
- Checked on a unit (test build v0.6.10): the power-on way, its strip, the eject and the restart, and unmounting and mounting the card on the computer. <!-- QA-GLB-015, QA-GLB-016, QA-GLB-017, QA-GLB-019, QA-GLB-020, QA-GLB-021 --> <!-- QA-CFG-200 -->
- A bench run wrote to one SD card at about 430 KiB/s. That is the rate of the unit's card writes on that card, not a speed you will see copying files, and it was measured on one card brand. <!-- docs/specs/OS-USBDISK.md section 13, 4a -->

## See also

- [File](file.md): projects, saving and loading, which the card needs back.
- [Config](config.md): the USB page, where **CARD AS DISK** and **READ ONLY** are.
- [Getting started](getting_started.md#use-the-card-on-a-computer): the power-on way in the first-start steps.
- [Panel](panel.md#keys-held-when-the-unit-turns-on): the keys held at power-on.
