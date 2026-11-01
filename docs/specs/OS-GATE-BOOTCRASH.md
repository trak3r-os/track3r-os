# OS-GATE-BOOTCRASH: the on-device update validator refuses an image not padded to a 4 KiB sector

Level C (the firmware update path). New Community OS behaviour; the rule is OBSERVED on the unit (OS-BUG-COSMBOOT, 2026-10-31). No persistent format changes: no on-disk layout, bootloader, linker script or image layout is touched, and nothing is on the audio path.

## Intent

The SD bootloader leaves a short final 4 KiB flash sector neither erased nor programmed (`docs/hw/tracker-hardware-notes.md` section 3). An image whose end (last populated address + 1) is not a multiple of 4096 is therefore installed with the bytes of the previous image in its tail. `v1.0.1-cosm.1` crashed at boot that way (OS-BUG-COSMBOOT). Our images are padded since fd83e9cd and f0fa2962 (`.flashpad`), and `check_image.py` has a 9th check, `sector_end`. The published v1.0.0 image is not aligned: it ends at 0x000E28BC (`check_image.py` on the published v1.0.0 release file, 2026-11-01).

The build gate and `o3console.py` already refuse an unpadded image on the host. The on-device validator did not: a file copied to the card by a computer, or stored by an older build, could still be armed and handed to the bootloader. This slice applies the same rule there.

- `HexValidator::finish()` ends with the sector rule: `image_length() % kFlashSector != 0` fails with the new `Fault::NotPadded`. It is the LAST verdict, so an image with another defect (SP, vectors, configuration) still reports that one. The fault offset is the end of the HEX stream (a byte count, not an image address), so the Update screen does not show it for this fault: `CheckStatus::offset_known` is false (set by `FwServiceSink` for `NotPadded`), the line has no " AT <offset>"; the console and the tests keep the value.
- `Fault::NotPadded` is appended before `Count`; no existing value moves. Its text is `image not padded: may not boot`.
- Every door runs the validator, so every door refuses it: `fw put` (at the end of the stream, the temporary file is dropped, nothing is renamed), `fw arm` (`ERR image not padded: may not boot`, nothing armed, `_fwinfo` not written), the Update screen's check (`ui_check_begin`: state Failed, line `NOT A VALID IMAGE: IMAGE NOT PADDED: MAY NOT BOOT`, 49 characters of the 86 the dialog's line holds), and the re-check inside the install (`ui_install` re-validates, so a file swapped after the check is refused too).
- `fw_sink.cpp`: like every other image fault, `NotPadded` reaches the port as `Refusal::Io` through the default of `refusal_of`; the fault text is what the screen shows (the check's Failed state), so no new `hal::firmware::Refusal` value and no change to `app/` is needed. The mapping table test (`mk66_update_client_test`) has a row for it.
- Test support: `good_image()` of `mk66_update_support.h` now pads with zeros to the next 4 KiB boundary, like `.flashpad`; `good_image(n, false)` keeps the exact length for the tests of the rule.

## Risk

- An image that was installable before is refused if it is not padded: the published v1.0.0 and every build before fd83e9cd. They can still be installed by the bootloader's own path (the reset button takes the first `.ptf` of `Firmware/`; the official update menu), which is not changed. They are exactly the images that may crash at boot.
- A refusal changes nothing: no `_fwinfo`, no pin, no token. The check is pure and runs before the hand-over; on the unit the hand-over is unchanged.
- The cost is one compare and one fault value (a few dozen bytes of .text); no allocation, no loop, nothing in an ISR.
- Hardware check still needed: the Update screen on the unit with a deliberately unpadded `.ptf` shows the line and installs nothing (QA-CFG-268). A host test is not hardware verification.

## Decisions

| # | Question | Decision | Who |
|---|---|---|---|
| 1 | Refuse an unpadded image on the device, or only warn? | Refuse, before arming. A warning would still let the unit crash-loop. | orchestrator |
| 2 | Exempt the published v1.0.0 (or any file name)? | No exemption by file name. The bootloader path (reset button, official update menu) is still there for older images. | orchestrator |
| 3 | Where in the verdict order? | Last: the more specific defects (SP, reset, vectors, configuration) keep their own message. | engineer (local, testable, reversible) |
| 4 | A new `hal::firmware::Refusal` for it? | No: the check reports it as a Failed state with the fault text, like every other image fault; the text carries the reason. | engineer |
| 5 | Crash-loop guard (after a crash record, boot safe with USB and the console only) | Not here: a product change, the IDEA row OS-BOOTSAFE for the maintainer. | orchestrator |

## Acceptance

- An aligned image passes; an image 1 byte short of a boundary (and 1 byte past one) is refused; the boundary exactly is accepted (`mk66_update_hex_test`).
- Nothing is armed, written or handed over on a refusal: `fw put`, `fw arm`, `ui_check_begin` + `ui_install` (`mk66_update_transfer_test`, `mk66_update_client_test`).
- The screen line fits the dialog (`firmware_update_test`).
- `check_image.py` and the validator agree on the same files (`mk66_fw_hex_cross`).
