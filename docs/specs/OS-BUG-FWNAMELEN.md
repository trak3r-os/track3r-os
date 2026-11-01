# OS-BUG-FWNAMELEN: never arm an update for a file name over 30 characters

Level C (the firmware update path). New Community OS behaviour; the limit of 30 is OBSERVED, the bootloader's exact rule is an open research question. No persistent format changes.

## Intent

On the unit (2026-10-31, `docs/hw/tracker-hardware-notes.md`, row "File name length"), the SD bootloader flashed the FIRST `.ptf` of `Firmware/`, with no error and no crash, when the file named for the update had a 33-character name (2 of 2 attempts). Names of 21, 29 and 30 characters installed. The OS accepted names of up to 52 characters, so its update path could install a different image than the one the user chose, silently.

Fix, the smallest that closes it:

- One constant, `hal::firmware::kUpdateNameMax = 30` (the name with `.ptf`, so a stem of at most 26). The guard (`path_guard.h`: `update_name_ok`), the dialog and the tests use it.
- A valid image name may still be stored, listed and read (`valid_image_name`, `readable_name`: the 52-character rule is unchanged, so `fw put` and the list work as before). It is never ARMED: `FwService::begin_check` (the one door of `fw arm` and the screen's check), `ui_check_begin`, `fw arm` and `FatFirmwareStore::write_fwinfo` (the last guard before `_fwinfo`) refuse it with `Fault::NameLong` (`name over 30 characters`, port: `Refusal::NameTooLong`).
- The Update screen (Config, INFO, `F8`) lists such a file; `F1` UPDATE on it does not ask question 1, the dialog's line reads `NAME TOO LONG: 30 MAX`.
- `o3console.py`: `NAME_RE` is the 30-character rule; `--put` (the file's own name or `--name`) and `--update` refuse a longer name with a reason before the port is opened.
- `.claude/skills/fw-gate-check/SKILL.md` step 6 and the manual say: at most 30 characters on the card.

## Risk

A user whose file has a long name gets a refusal message (screen, console, o3console) instead of a silent flash of another image. Nothing is written by a refused call. A file that was copied to the card by hand with a long name can still be flashed by the hidden reset button or the official menu; that path is the bootloader's, not the OS's (the manual says to keep the name short). The limit is the longest proven length: a name of 31 or 32 characters may well work on the unit and is refused anyway (the conservative side). Hardware check still needed: a 30-character name installs from the screen and a 31-character name is refused (QA-CFG-266, QA-CFG-267).
