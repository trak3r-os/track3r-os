# OS-BUG-UPDFOREIGN: the device updater installs any well-formed image

Level C (the firmware update path), deliberately a small change. New Community OS behaviour. No persistent format changes.

## Goal

The on-device updater (Config > INFO > F8, console `fw` and `--update`) refused every official firmware image: `bad reset vector` (the official reset handler sits at 0x1D0, below the flash configuration, a normal layout), a vector table with non-zero reserved slots, vectors into SRAM, a different initial SP. The Update screen should install an official `PolyendTracker_*.ptf` too.

## Decision

Maintainer, 2026-11-18: "install anything, recovery by reset button". The hidden reset button flashes any good `.ptf` from the bootloader whatever the application does, so the device does not judge the application's layout.

## Kept checks, and why

| Check | Why it stays |
|---|---|
| Intel HEX record syntax, checksum, types 00/01/02/04, EOF last | a malformed stream cannot be flashed faithfully |
| addresses <= 0xFFFFF, no byte twice, contiguous from 0, `0x0..0x40F` present | the image must fit the flash and be one run |
| flash configuration `0x400..0x40F` exactly `ffffffffffffffffffffffffdef9ffff`, FSEC & 3 == 2 | a wrong field can lock the MCU for good: unrecoverable, not even the reset button helps |
| image end <= 0xFF000 (`LastSector`) | an image that filled the last sector hung the unit (OS-BUG-LASTSECTOR) |
| image end on a 4096 boundary (`NotPadded`), with the PAD offer of OS-UPDPAD unchanged | the SD bootloader leaves a short final sector stale; an official image now has only this fault and gets the PAD offer |

Removed on the device: the exact initial SP, the reset vector rule, the vector table rules (`BadSp`, `BadReset`, `BadVector` are no longer raised; the enum values stay so that none moves).

## The gate for our own images stays strict

`check_image.py check` keeps every check (profile `build`, the default). `check --profile device` mirrors the device's relaxed rules; `fw_hex_cross.py` (ctest `mk66_fw_hex_cross`) compares the device validator against that profile. `o3console.py --put` pre-checks with `--profile device`, so a foreign image is not refused on the host either.

## Done when

- `mk66_update_hex_test` and `mk66_update_client_test` pass: the old SP, reset and vector cases are accepted; a synthetic official-shaped image (reset 0x1D1, reserved vectors non-zero, an SRAM vector, another SP) has only `NotPadded` and validates once padded; the flash configuration cases fail as before.
- `check_image.py --self-test` and `mk66_fw_hex_cross` pass.
- Hardware check still open: install an official image through the Update screen on a unit (PAD, then INSTALL), then return with the reset button. QA-CFG-283.
