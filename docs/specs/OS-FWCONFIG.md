# OS-FWCONFIG: firmware update from a Config page

Status: spec, 2026-10-18. Backlog row 9. **Level C** (the update path). The branch merges only after a hands session with the maintainer on the unit. This is **new Community OS behaviour**.

The Q rows below were **decided by the orchestrator under the maintainer's delegation, 2026-10-18**, each with the simplest option. The maintainer checks them when reading this spec.

## Intent

Today the only way to update is the console: `fw put`, then `fw arm`, then `fw update` (OS-006c, `platform/mk66/update/include/o3os/platform/mk66/update/fw_service.h`). This item lets the user update from the unit itself:

- pick a `Firmware/*.ptf` that is already on the card;
- confirm twice;
- the OS re-validates it, the same way the console does;
- the OS hands over to the bootloader through **the same code** the console uses.

It adds no new way to write flash. The bootloader still does the flashing, from `Firmware/` and `_fwinfo`, as today.

## What exists

- **`HexValidator`** (`.../update/hex_validator.h`) is a streaming check fed in any chunking. Its rules are those of `tools/check_image.py`, plus ascending records. It checks the flash configuration field exactly, the SP, the reset vector and the vectors, and reports the image length and the file's CRC-32.
- **`FwService`** holds the console state machine.
  - `arm` re-reads and fully re-validates the file, checks its CRC, and issues a one-use token valid for 30 s.
  - `update <token>` re-validates once more, then does the hand-over:
    1. stops the transport and mutes the outputs (`FwHost::quiesce`);
    2. writes `Firmware/_fwinfo` with the image name and reads it back;
    3. drives the bootloader request pin low;
    4. if the chip is still running after 5 s, releases the pin, deletes `_fwinfo` and reports an error.
  - The verification is sliced: 2 KiB a main-loop pass (`verify_slice`).
  - The methods all take a `usb::diag::Shell&`.
- **`FirmwareStore`** lists, reads and writes `_fwinfo` (`firmware_store.h`). Only `fat_firmware_store` may name write-class FatFs symbols (`tools/check_layers.py` rule 6).
- **Refusals:** the update path already refuses while the card is on a computer (`FwHost::card_on_host`, `host_sessions`) or storage is busy.
- **Config** has pages F1-F6 (MIDI, DISPLAY, SD, INFO, USB, PATTERN), F7 APPLY, and F8 unused (`app/include/o3os/app/screens/config_screen.h`); slice 2a gives F8 the label UPDATE on INFO. INFO shows the product and the build id.

## Behaviour

1. **The entry point.** On Config **INFO**, **F8 UPDATE** (that page only; see Q1) opens the **firmware list**, a modal dialog:
   - every `*.ptf` of `Firmware/`, in directory order, with name and size;
   - the first one is marked `RESET TARGET`, the reset button's target;
   - a name that does not fit before the tag (from 50 characters with a size of four digits; the longest name the port takes is 52) is cut with `...` on that row only; the question shows it whole;
   - `_o3up.tmp` and `_fwinfo` are hidden;
   - an empty folder says `NO .PTF IN FIRMWARE/`.

   Up and Down move. F1 UPDATE, F8 BACK.
2. **Refusals.** The update is refused, and the strip names the reason, when:
   - the card is on a computer;
   - storage is busy;
   - an export or a recording is running;
   - the project has unsaved changes (`SAVE THE PROJECT FIRST`);
   - a console `fw` operation is in progress (one owner; `FwPhase` is not Idle or Done);
   - the chosen file's name has more than 30 characters (`.ptf` included): `NAME TOO LONG: 30 MAX` on the dialog's line, before question 1 (OS-BUG-FWNAMELEN, `docs/specs/OS-BUG-FWNAMELEN.md`; the SD bootloader flashed another file for such a name).
3. **Question 1** (an OS-KEYROWDLG question): `UPDATE TO <name>? F1 YES`. Any other key cancels it.
4. **The check.** The file is read and validated on the card again, sliced at 2 KiB a pass. A progress dialog shows `CHECKING <name>` with a bar, and F8 CANCEL.
   - **If it fails**, the dialog's own line says `NOT A VALID IMAGE: <fault> AT <offset>` (amended 2026-10-19: once the dialog is open, a failure or a refusal shows on its line, not on the strip). Nothing is written, and the dialog returns to the list.
5. **Question 2:** `IMAGE OK, <length> B, CRC <8 hex>. F1 INSTALL`.
   - It is valid for 30 s, the console's token lifetime. Any other key, the timeout, or a hand-over of the card to a computer cancels it.
   - The CRC is the full file's, as `fw arm` prints it.
6. **The hand-over.** F1 INSTALL runs the console's `update` path exactly: re-validate once more, quiesce, `_fwinfo` written and read back, then the pin.
   - The screen shows `UPDATING: DO NOT POWER OFF`.
   - If the bootloader does not take over within 5 s, the pin is released, `_fwinfo` is deleted, and the dialog's line says `UPDATE DID NOT START` (amended 2026-10-19: the dialog's line, as in rule 4; the strip names only the refusals that stop the dialog from opening).
   - The refusals of rule 2 about an export, a recording and unsaved changes are asked again at INSTALL (the dialog is modal, but a recording or an edit can still come from outside while question 2 waits); a refusal there cancels the check and shows on the dialog's line.

## The port

- **`hal/include/o3os/hal/firmware.h`** defines a new **`FirmwareSink`** port, used by the App only:
  - `available()`: false where there is no update path;
  - `list_begin` / `list_next` / `list_end`: name, size, first flag;
  - `begin_check(name)`;
  - `poll_check()`: one slice; it returns Running with progress, Ok with length and CRC, or a Fault with an offset;
  - `cancel()`;
  - `install()`: does not return on success.
- **On mk66** the port is an adapter over `FwService`. `FwService` gains a **shell-free client API** (`ui_check_begin`, `ui_check_slice`, `ui_install`). It shares `verify_slice`, `handover_check` and `begin_handover` with the console, so there is **one hand-over implementation**. The 30 s token rule is internal to that API.
- **The host and the simulator** get a fake sink: a fixture list, scripted check results, and an `install()` that only records the call.

## Decisions

| # | Question | Decided (simplest) |
|---|---|---|
| Q1 | Where the entry lives | **F8 on the Config INFO page only, labelled UPDATE**, opening a list dialog; no new page, no row. (Amended 2026-10-19 by the orchestrator under the maintainer's delegation: F1..F6 on Config are the page keys and INFO has no row cursor, so the earlier "a last row `FIRMWARE UPDATE`, F1 on it" cannot be used.) F8 stays unused on the other pages |
| Q2 | The CRC in the list | the list shows name and size only; the CRC comes from the check and is shown in question 2. Listing a CRC for each file would read every image in full, several seconds each; this deviates from the backlog row's "name, size and CRC" |
| Q3 | Unsaved changes | refuse, with `SAVE THE PROJECT FIRST`; no save-and-update |
| Q4 | Confirmations | question 1 (F1 YES), then the check, then question 2 (F1 INSTALL) within 30 s; any other key cancels |
| Q5 | Code shape | a shell-free API in `FwService`, one shared hand-over; no second implementation in the adapter |
| Q6 | Files listed | every `*.ptf` in directory order, the reset target marked; temporary files hidden; no delete from this dialog |
| Q7 | Simulator | a fake sink, so the dialogs have goldens; no flash access anywhere off the unit |

## Risks

1. **Bricking.** The flash configuration field and the vectors are checked by `HexValidator`, the same rules as `check_image.py`. The UI must not skip the re-validation in `install()`.
2. **A second hand-over path.** Duplicating `begin_handover` could make the two drift apart. Q5 forbids it, and a test calls both entry points on the same fake.
3. **The card goes to a computer or is pulled during the check.** A dead `host_sessions` count or a read fault aborts the check. Nothing is written before question 2.
4. **Stale `_fwinfo`.** It is deleted before it is written, read back, and deleted if the hand-over fails. That is the console's behaviour, shared.
5. **Audio during the hand-over.** `quiesce()` stops the transport and mutes the outputs before the pin, as today.
6. **An accidental update.** Two questions, each answered only by F1, with a timeout. The list is reached only from a Config row.
7. **Console and UI at once.** One owner: either refuses while the other's `FwPhase` is active.
8. **Power loss during the hand-over.** This is the bootloader's domain, the same as the console path. The screen tells the user not to power off.
9. **The layer rules.** The App sees only `FirmwareSink`. `check_layers.py` stays green, and no FatFs symbol leaves `fat_firmware_store`.
10. **Flashing during development.** The engineer never runs `install()` on hardware. Only the hands session with the maintainer does, after `fw-gate-check` on the image (CLAUDE.md hard rules).

## Acceptance (Done when)

1. **Host tests:**
   - the list (order, the first flag, hidden files, an empty folder);
   - every refusal;
   - question 1 and question 2, with cancel by another key and by the 30 s timeout;
   - a check that passes, a check that fails (fault and offset), and a cancelled check;
   - nothing written before INSTALL;
   - `install()` reaching the shared hand-over;
   - console and UI exclusion.

   The UI side uses the fake sink, and `FwService`'s client API runs with the fake store, as `mk66_update_hex_test` does.
2. **QEMU:** `tools/ci.sh qemu` runs the `FwService` client API on the target build.
3. **Simulator:** snaps of the INFO row, the list, both questions, the progress dialog and a failure. `budget_test` stays under 1792.
4. **QA and docs:**
   - QA rows QA-CFG-040..059 in `docs/qa/catalogue/config.md`;
   - the ui-map Config row;
   - `docs/manual/install.md` gets an "Update from the unit" section, marked unit-pending;
   - the CHANGELOG Unreleased entry.
5. **Gates:**
   - `tools/ci.sh` full;
   - `check_layers.py` green;
   - FLASH and SRAM deltas in the commit body.
6. **Hardware (the hands session, before the merge):**
   - an update from a gate-checked `.ptf` to a known build, with the build id confirmed after the reboot;
   - a corrupt `.ptf` refused;
   - a refusal with the card on the computer;
   - the "did not start" path, if the maintainer wants it tested.

## State for implementer

- **Branch:** `feat/OS-FWCONFIG`. Level C: a risk review plus one reviewer. **Do not merge**: it waits for the maintainer's hands session.
- **Read first:**
  - `fw_service.h` (its header comment is the console contract);
  - `fw_service.cpp` (`verify_slice`, `handover_check`, `begin_handover`);
  - `hex_validator.h`;
  - `firmware_store.h`;
  - `app/include/o3os/app/screens/config_screen.h`;
  - `app/include/o3os/app/key_prompt.h` (questions and dialogs).
- **Steps:**
  1. `FwService` client API, with host tests on the fake store.
  2. `hal/.../firmware.h` `FirmwareSink`, the mk66 adapter, and the fake sink for host and sim.
  3. The INFO row, the list dialog (a new OverlayId and Context), the two questions and the progress dialog, with app tests.
  4. Sim snaps, QA rows and docs.
- **Rules:**
  - no new flash-writing code;
  - no change to `HexValidator`'s rules;
  - key names come from `key_prompt.cpp`'s tables (`check_keytext.py`);
  - never call `install()` on hardware yourself.
- **Size [est]:** FLASH +3-4 KB; SRAM 0. The check reuses `FwService`'s 2 KiB buffer, which already exists. CPU per audio block 0.
- **Stop and report** if:
  - the shared hand-over cannot be reached without a shell;
  - a check needs more than one 2 KiB buffer;
  - anything would touch the bootloader request pin outside `begin_handover`.
