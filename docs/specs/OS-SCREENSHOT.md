# OS-SCREENSHOT: Shift + Power saves a screenshot to the card

Status: spec, 2026-10-19. Backlog row 5, APPROVED by the maintainer 2026-10-17. It is lane B's last item. **Level B**: no format change; a new file type on the card, written through the existing write path. This is **new Community OS behaviour**.

The Q rows were **decided by the orchestrator under the maintainer's delegation, 2026-10-19**, each with the simplest option.

## Intent

Shift + Power saves what the screen shows as `Screenshots/SHOT_nnnn.bmp` on the card. It reuses the console `shot` capture (OS-HIL), costs no SRAM, and has no real-time impact.

## What exists

- **The console `shot`** (`platform/mk66/README.md:1038, 1060-1066`; `usb/shot_stream.h`).
  - The FT812 coprocessor runs `CMD_SNAPSHOT2` in RGB565 into the spare RAM_G region, in two bands of 240 rows.
  - The region is `ram_g_map.h` `kRamGShot`: from 0x89000, 476 KiB (after the 96 KiB note map of OS-SONG-VIEW2 V4, which took 0x71000..0x89000 of what was 0x71000, 572 KiB), less the DMA self-test's last 64 bytes. No bitmap, font or wave slot is touched.
  - The main loop reads each band back over SPI, only when `Ft812Display::bus_idle()`, and a few lines a pass. Audio, the display and the console never stall.
  - A key press or 3 s without progress ends it.
- **The Power key** (OS-POWER, `docs/specs/OS-POWER.md` D5, `core::PowerHold`).
  - Shift down when Power goes down: the state is `Spent`, and nothing happens. The gesture is **reserved for this item**.
  - Shift pressed during a hold cancels the power-off hold.
  - Power is not part of any boot chord.
- **Card claims:** `platform/mk66/storage/include/o3os/platform/mk66/storage/card_claims.h`. Card on the computer: OS-USBDISK.

## Behaviour

1. **The trigger.** `PowerHold` reports a new one-shot event, `Screenshot`, when Power goes down (debounced) while Shift is already down. This is exactly the D5 `Spent`-at-press case. Everything else is unchanged:
   - Shift pressed *during* a hold still only cancels the hold;
   - a plain hold still powers off;
   - after the power-off commit (D2) nothing is taken: a screenshot armed or running at the commit is cancelled (the temporary file removed, the card released, no strip text).
2. **When the picture is taken (Q2).** The request is armed at the press. The picture is the **first frame drawn after both Shift and Power are released**, so the key row shows the screen's plain layer and not the Shift labels.
3. **Refusals.** The strip says `NO SCREENSHOT: <reason>` and nothing is captured when:
   - there is no card;
   - the card is on the computer;
   - storage is busy (a save, an export, a recording, a project load, or a `fw` phase that holds the card);
   - a console `shot` is running, because it uses the same RAM_G region;
   - a screenshot is already in progress.
4. **The capture.**
   - The display list is held (no new frame is swapped in) from band 1's snapshot until band 2's has been taken, so the two halves come from one frame. Audio goes on.
   - Each band goes `CMD_SNAPSHOT2` (RGB565, w 800, 240 rows) → RAM_G. It is read back into a 2 KiB buffer, at most one buffer a main-loop pass and only when `bus_idle()`, and that buffer is written to the open file.
   - The screen is frozen for about one to two seconds [est]. The strip then says `SCREENSHOT SAVED: SHOT_0007.BMP`.
   - **Input (Q4).** Presses of keys and pads and encoder turns that arrive during the capture are dropped. The release of a key that was down before it began is kept and delivered after it, so no note and no hold hangs; the release of a key whose press was dropped is dropped too, whenever it comes.
5. **The file (Q1).**
   - Format: a BMP, 16-bit RGB565 (`BI_BITFIELDS`), top-down (negative height), 800 × 480, 1,600 B a row: a 66 B header plus 768,000 B.
   - Place: `Screenshots/`, created if missing. The name is `SHOT_nnnn.bmp`, the next free number (0001-9999), and a file is never overwritten (the export naming's NewOnly rule). With 9999 used: `NO SCREENSHOT: FOLDER FULL`.
6. **An abort.** A key press does **not** abort it (unlike the console `shot`). These do:
   - a card fault;
   - the card being handed to the computer;
   - a display reset;
   - 3 s without progress.

   On an abort the partial file is deleted, and the strip says `SCREENSHOT FAILED`. The platform also cancels a screenshot silently (same clean-up, no strip text) at the power-off commit and when the display or the app has failed.

## Decisions

| # | Question | Decided (simplest) |
|---|---|---|
| Q1 | Format | BMP RGB565, top-down: a fixed 66 B header and the snapshot bytes as they come, so no conversion and a few hundred bytes of code. PNG needs deflate and CRC framing, more flash, and RGB888 conversion |
| Q2 | Which frame | the first frame after Shift and Power are both released, so the plain key row shows |
| Q3 | Folder and name | `Screenshots/SHOT_nnnn.bmp`, next free number, never overwritten |
| Q4 | Keys during capture | ignored for the capture's 1-2 s; the screen is frozen; only the aborts of behaviour item 6 stop it. Decided by the orchestrator under the maintainer's delegation, 2026-10-19: presses (keys, pads) and encoder turns that arrive during the capture are dropped, not queued; releases of keys that were already down are kept and delivered after it (`usb/input_hold.h`) |
| Q5 | Where the code lives | a platform `ScreenshotService` beside the console `shot`, sharing its band reader. The App supplies only "may I" (storage and claims) and the strip message, through the existing app/platform interface. The simulator has a host version that writes its own frame buffer, for tests |
| Q6 | The colour artefact | accepted and documented: captures have read RECTS fills at about 0.545 of their colour (OS-SIMKEY, OS-SIMKEY2: `pclk0` changed nothing). It is a known issue of the capture, not a blocker |

## Risks

1. **Sharing RAM_G with the console `shot`.** The two exclude each other (behaviour item 3).
2. **The Power key.** The new event must not change OS-POWER's states. `PowerHold`'s tests must keep every existing case, and add Shift-then-Power giving `Screenshot` and no hold.
3. **A frozen display list during the capture.** Frames wait; nothing else does. Audio runs from ISR and DMA, and the main loop only skips swaps.
4. **The card.** The capture takes a card claim for the file's life. A pull or a hand-over aborts it and deletes the partial file, as the export does.
5. **Shift + Power during an export, a save or a load.** It is refused, never queued.

## Budget [est]

- **FLASH:** +2-3 KB (the service, the BMP header, the naming, the `PowerHold` event).
- **SRAM:** 0. The 2 KiB buffer lives in SDRAM, or reuses the `shot` reader's.
- **RAM_G:** 0 new; the existing `kRamGShot`.
- **CPU per audio block:** 0. The main loop does about 1 ms of polled SPI per pass while capturing.

## QA rows

Global catalogue, QA-GLB-130..139:

- **130:** Shift + Power saves `SHOT_0001.bmp`, and the strip says so.
- **131:** the next shot is 0002, and nothing is overwritten.
- **132:** the picture shows the plain key row, not the Shift row.
- **133:** refused with no card.
- **134:** refused with the card on the computer.
- **135:** refused during an export or a save.
- **136:** refused while a console `shot` runs.
- **137:** a plain Power hold still powers off; Shift during a hold still cancels it.
- **138:** the song plays on during a capture, by ear.
- **139:** a card pulled during a capture leaves no partial file.
- **140:** keys, pads and the encoder pressed during the freeze do nothing; a pad held when it began still ends cleanly.

## Done when

1. **Host tests:**
   - `PowerHold` with the `Screenshot` event (every OS-POWER case still passes);
   - the BMP header bytes;
   - the next-free-name rule;
   - every refusal;
   - an abort deleting the partial file;
   - the simulator's host service writing a BMP that matches its frame buffer.
2. **`tools/ci.sh host` and the `mk66` build:** green, with no warnings, and the FLASH and SRAM deltas in the commit body.
3. **Docs:**
   - the QA rows above;
   - the ui-map's global keys;
   - OS-POWER's D5 note ("reserved") points here;
   - the CHANGELOG Unreleased entry.
4. **Unit:** QA-GLB-130..139 by the tester. The file opens on a computer and shows the screen.

## State for implementer

- **Branch:** `feat/OS-SCREENSHOT`. Level B: one `firmware-reviewer`.
- **Read first:**
  - `docs/specs/OS-POWER.md` (D2, D5, the `PowerHold` table);
  - the console `shot` code and `usb/shot_stream.h` (the band reader, `shot_region`, `bus_idle`);
  - `card_claims.h`;
  - the export's NewOnly naming in `app/src/save_service.cpp` (about :767-830).
- **Steps:**
  1. Add the `Screenshot` event to `PowerHold`, with tests.
  2. Split the band reader out of the console stream, so both the console `shot` and the new service use it.
  3. Build `ScreenshotService`: refusals, the frame-after-release trigger, the held display list, the BMP writer and the file naming.
  4. Add the strip messages through the App.
  5. Add the simulator's host service and its tests.
  6. Docs and QA rows.
- **Rules:**
  - no change to OS-POWER's power-off behaviour;
  - no new RAM_G region;
  - no SRAM buffer;
  - the screen freeze is the only visible side effect.
- **Stop and report** if:
  - holding the display list stalls anything but frames;
  - the band reader cannot be shared without changing the console `shot`'s output.
