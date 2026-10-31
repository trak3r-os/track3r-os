# Tracker OG hardware notes: unit differences and version behaviour

Status: 2026-10-11. Input for the Community OS HAL and for anyone flashing images. Facts are tagged **OBSERVED** (seen on the maintainer's unit during the hardware session of 2026-10-10), **CATALOG** (official release notes) or **UNKNOWN** (an open research question: a claim about the original firmware with no evidence kept in this repository is never stated as a fact). The maintainer's unit model, board revision and bootloader version are still not recorded (Gate A; catalog Q6).

## 1. Display

| Item | What we know | Tag | Consequence |
|---|---|---|---|
| Orientation | On the maintainer's unit an early test image with the FT812 rotation (`REG_ROTATE`) at 1 showed the screen **upside down**; the official 1.9.0 shows it correctly. **The track3r-os driver (2026-10-11, OS-HW1) is upright with REG_ROTATE 1 and upside down with 0** on the same unit, so the earlier observation needs another cause. | OBSERVED | Possible causes: later units mount the panel the other way, or the official firmware selects the orientation at run time. How the official firmware gets an upright image (board-revision strap/GPIO, a config value, a check of the panel) is UNKNOWN. The OS display HAL must make orientation a setting with a safe default, and detect the revision once its mechanism is known. |
| First boot after flashing | After the bootloader flashed an early test image (0.0.2), the first boot showed a corrupted display; a power cycle fixed it. Other test images (0.0.1, 0.0.9) did not do this. | OBSERVED | FT812 bring-up must survive a warm hand-over from the bootloader: full power-down/reset of the EVE chip, clock re-selection and register reinitialisation before the first display list. |
| Brightness noise | 1.3.2 fixed a high-pitched noise at medium/low display brightness (backlight PWM frequency). | CATALOG (PAR-066) | The backlight PWM frequency is audible on some units; keep it out of the audio band. |
| Themes and fonts | 1.6.0 added Display Font (Original/New) and Display Theme (Original/Monochrome). 1.9.2 added an anniversary theme. | CATALOG (PAR-097) | UI only, with no hardware difference. |

## 2. Memory, toolchain and placement

| Item | What we know | Tag |
|---|---|---|
| Audio block alignment | `audio_block_t` has 2-byte natural alignment. Under `-fdata-sections` the AudioMemory pool landed at `0x1FFF0606`, and the Audio library's LDM/LDRD on `block->data` raised an UNALIGNED UsageFault, escalated to a HardFault in the audio software ISR. One earlier build only worked because its pool happened to sit at `0x1FFF0608`. Fixed by `aligned(4)` on the type, plus a build guard. | OBSERVED |
| Fault behaviour of the core | The Teensy core's fault handler loops forever calling the USB interrupt handler. A crashed image therefore still enumerates over USB (CDC/MIDI/HID/MTP) but does nothing else, and the host resets it every few seconds. The unused-interrupt handler ends up in the same loop. | OBSERVED |
| SD init time | `SD.init` took about 2.0 s on the test card (FAT32, 15 GB) in the trace of 0.0.9. | OBSERVED |

## 3. Flashing, bootloader and recovery

| Item | What we know | Tag |
|---|---|---|
| Bootloader | It is separate from the application and flashes a `.ptf` (Intel HEX) from the SD card. This OS starts an update by writing the file name to `Firmware/_fwinfo` and pulling the bootloader request pin (Teensy pin 81, PTA1) low; on the maintainer's unit this hand-over flashed the named image (`fw update`, 2026-10-12: `platform/mk66/README.md`, "What the bootloader needs"). That the bootloader deletes `_fwinfo` afterwards, and how the pin and the file name were first learned, has no method recorded in the repository: open research question. A broken application image cannot remove the recovery path. How the official firmware starts its update is an open research question. | OBSERVED (the hand-over flashing the image); open research question (the rest) |
| Update menu file filter | **The 1.9.0 update menu lists only `.ptf` names starting with `PolyendTracker`.** Our test images were therefore named `PolyendTracker_0.0.x.ptf` (versions Polyend never published), identified by SHA-256. The filter of other versions is UNKNOWN. | OBSERVED |
| File name length | **The update installed the FIRST `.ptf` of `Firmware/` instead of the named file, with no error and no crash.** With `o3os_1.0.1-stereobase.1-bench.ptf` (33 characters) the unit came back on the older image `PolyendTracker_0.0.9.ptf`: 2 of 2 attempts. The same image named `o3os_1.0.1-stbase.1-bench.ptf` (29 characters) installed. Names that installed the same day: `o3os_1.0.1-cosm.3.ptf` (21), `o3os_1.0.1-stereo2.1-bench.ptf` (30), `o3os_1.0.1-stereo2.2-bench.ptf` (30). So the limit lies between 30 and 33 characters. The exact rule is an open research question (the bootloader was not examined). Tester, 2026-10-31. | OBSERVED (exact rule: open research question) |
| Hidden reset button | It flashes the **first** `.ptf` in `Firmware/` (with several files present it picked `PolyendTracker_0.0.4.ptf`). Leave exactly one `.ptf` in the folder before pressing it. This is how the unit was recovered from every hung image. The official FAQs: <https://polyend.ladesk.com/220365-How-do-I-update-Trackers-firmware>, <https://polyend.ladesk.com/929798-How-do-I-perform-the-emergency-flashing-procedure>. | OBSERVED + vendor FAQ |
| Image size | Every image tested fit; the largest ended at `0x000FE167`. No upper limit was hit. | OBSERVED |
| Short final sector | **The SD bootloader leaves a final flash sector that the image fills only in part neither erased nor programmed; the sector keeps the bytes of the image that was installed before.** Image `v1.0.1-cosm.1` ended at `0x000EA07B`, so its last 4 KiB sector (`0xEA000`) held 124 bytes, the tail of `.data`'s load image. After the update the sector still held the bytes of the older `v1.0.1-hpbp1c.1`: `g_link`'s vptr (display `device.cpp:287`, `.data` at `0x20000E70`, loaded from flash `0xEA004`) read that image's word `0x00023C1D`, and `display::Chip::init`'s virtual call (`0x14B86`, `blx r3`) jumped to cosm.1's word at `0x23C1D + 28` = `0x04ED8353`, exactly the PC of the unit's crash record (BusFault IBUSERR, LR `0x14B89`). Images that end with a longer tail (`stereo.1`, `g_link`'s vptr at `0xE9F1C`) booted. Evidence: the crash record of the unit, 2026-10-31 (OS-BUG-COSMBOOT). The image generator therefore pads the image with zero bytes (not `0xFF`, the erased value, in case the bootloader skips erased-looking content) to a 4 KiB boundary (`.flashpad` in `mk66.ld.in`) and `check_image.py` refuses an image that does not end on one (`sector_end`). | OBSERVED |
| Bootloader rule for a short final sector | The exact rule is **UNKNOWN**. The data fit two forms: (a) it skips a final sector holding fewer than T bytes of the image, with 124 < T <= 812 (the bounds the observed images give); (b) it programs in 128-byte units and rounds the last one down. Units of 256 B or more are ruled out (`stereo.1` boots). Whether a hole inside the image, or an erase of the sector, behaves the same way is not tested. The padding makes the question moot for our images; a research session could settle it with a sweep of tail lengths. | UNKNOWN (open research question) |
| USB identity | Our test images enumerated as VID `16d0`, PID `0e9f` with a placeholder product string and serial `4294967295` (not unique). **The VID/PID is the official firmware's pair, kept by the maintainer's decision (2026-10-16, confirmed 2026-10-18), publication included; the maintainer's statement is the evidence. The OS is told apart by `bcdDevice` and the strings** (ADR 0012 amendment 3). Official 1.9.0 enumerates as vendor `Polyend`, model `Tracker`. | OBSERVED |

## 4. Configuration and EEPROM

| Item | What we know | Tag |
|---|---|---|
| Config across versions | The OG config structure grows per release (theme/font, numbering, pattern arrangement, top info, line-in channel, clock sync…). The vendor tells users to create a new project after an update so Config initialises. | CATALOG (PAR-010) |
| 1.1.14 reading a 1.9 EEPROM | Whether 1.1.14 keeps or resets the settings of a 1.9 config after a downgrade is not known (not tried on the unit). | UNKNOWN |
| Radio region | 1.4.0 added a Japanese FM band (Config "Radio Region"), applied without restart from 1.5.0. | CATALOG (PAR-085) |

## 5. Audio I/O

| Item | What we know | Tag |
|---|---|---|
| Line-in | 1.4.0 added mono line-in passthrough with delay/reverb sends; 1.8.0 updated the "Line-in channel" description. The SGTL5000 delivers stereo; the catalog calls the 1.4.0 passthrough mono. Whether the board wires both channels is to be verified (vision doc). | CATALOG + vision |
| Engine rate | Whether the official firmware's audio clock is 44,100 Hz or slightly above it (44,117.65 Hz, the rate this OS gave before v1.0.0, is not known to be the same) and how its export labels the rate is an open research question: no measurement of the unit's output rate is recorded here. This OS's own rate: exactly 44,100 Hz by default since v1.0.0 (D-OS-8, verified on the unit 2026-10-23: 44,100.00 Hz measured); `platform/mk66/README.md`, MCLK. | UNKNOWN |

## 6. Storage and USB features by version

| Version | Change | Tag |
|---|---|---|
| 1.2.0 | MTP file transfer over USB (SD browsing from a computer). | CATALOG (PAR-022) |
| 1.4.0 | MTP disabled for reliability; power-off without an SD card fixed. | CATALOG (PAR-086, PAR-088) |
| 1.3.0 | NES ROM player from `/Games` (out of scope by maintainer decision). | CATALOG (PAR-060) |
| 1.7.2 | Hidden device-diagnostic screens (buttons/pads/encoder, screen, RAM, SD report); battery hardware variants are UNKNOWN (catalog Q6). | CATALOG (PAR-132) |
| 1.8.0 | Recording up to ~133 s (memory map change). | CATALOG |
| 1.9.0 | 12 tracks (4 MIDI-only), 2,320-byte projects, PAMD names file, `.pti` instruments in project folders. | CATALOG + fixtures |

## 7. SD card handling on the host (practical)

- The Tracker needs a single FAT32 partition (MBR). An ext4/Raspberry-Pi-style card cannot be used without reformatting. **OBSERVED**
- On some laptops the built-in reader (sdhci-pci) hangs after a card is removed while mounted or busy (`mmc0: tried to HW reset card, got error -110`). Fix: reinsert, or `sudo modprobe -r sdhci_pci && sudo modprobe sdhci_pci`. Always eject from the file manager first. **OBSERVED**
- Any SD card may be written (maintainer, 2026-10-17: no marker file, no volume check), but format a card, or delete files you did not put there, only when the maintainer asks. Never use raw block-device writes, debug probes or improvised uploaders.

## 7b. The Power switch and what "off" is (OS-POWER, `docs/specs/OS-POWER.md` section 2)

- The rear Power switch is on Teensy pin 9 = PTC3, a plain input; the tree's early-boot table lists it as an input that is never driven. **No power latch, enable or kill output is known** on the board (the pins the tree drives early are PTD6, PTA24, PTC19, PTD15 / PTD11, PTE26, PTE27, PTE24, none of them a latch); a kill line driven only at power-off would not be in that table, so the docs cannot exclude one. **OPEN** (measurements M1 to M3 of the spec, a multimeter and a USB power meter, optional).
- The Tracker has no battery (5 V / 1 A over USB-C; **CATALOG**/public support pages), so whatever the OG's "off" is, it happens with the supply plugged in. The Community OS does not know whether the OG cuts its own rail or sleeps deeply. Its own off is built as the fallback: the peripherals are put in their shutdown, the unit goes dark and silent, the SD card and the codec stay powered; **not** electrically off (unplug the cable to remove the power). Slice P1 ends in a restart; the low-power state is slice P2.
- The polarity of PTC3 and its pull are the board's two unknowns: `diag power` prints the raw level with no pull, the pull-up and the pull-down, idle and pressed (QA-GLB-113). **OPEN** until the first unit session.
- What the bootloader does with a button held at a reset is not known (the hidden reset button flashes the first `.ptf` of `Firmware/`); the OS therefore restarts only after Power has been released. **OPEN**.

## 8. Open questions (maintainer or a later hardware session)

1. Board revision and how official firmware picks the display orientation (strap, GPIO or config).
2. Unit model, serial and bootloader version (Gate A record).
3. Battery-powered variants (Q6) and whether they need different power management.
4. Stereo line-in wiring and the SGTL5000 DAP features actually usable in-codec (vision doc "verify" items).
