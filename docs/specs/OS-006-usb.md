# OS-006 — USB stack (ADR 0012)

Status: 006a merged; 006b specified 2026-10-11 · Source of truth: `docs/adr/0012-usb-audio.md`, including its "Amendments at acceptance", which override the body.

## 006a — Host-pure USB library: descriptors, packetiser, feedback, rings (ADR 0012 slices S1 + S2)
**Scope:**
- **New library `usb`,** depending on `core` only. `hal` gets a `UsbDevice` contract header only if the packetiser needs one; there is no MK66 driver yet (that is slice S4, 006b). Add it to `check_layers.py`.
- **Descriptor builder.** Builds the device, configuration, IAD and interface/endpoint descriptors for:
  - modes: STANDARD (MIDI + CDC `diag`) and AUDIO (UAC2 + MIDI + CDC);
  - speeds: Full, and High as a parameter only;
  - profiles: 2, 8, 10, 11, and 4S if cheap.

  The descriptors are built at compile time (`constexpr`) or into a fixed buffer at boot. String descriptors carry the per-channel names (S1..Sn, Master L/R), the serial number placeholder of ADR 0011 item 6, and VID/PID from one header (the official firmware's pair, kept by the maintainer's decision (2026-10-16, confirmed 2026-10-18), publication included; the OS is told apart by `bcdDevice` and the strings).
- **Structural validator** (it is also used by the tests): lengths, `wTotalLength`, interface/endpoint numbering, IAD, `wMaxPacketSize` from channels × 2 B × 45, ≤ 1023 at FS, alt 0 with no bandwidth, at most 15 endpoint numbers, and per-mode endpoint counts.
- **UAC2 class requests** as pure functions that work on request structs: clock source `GET CUR`/`GET RANGE` (one discrete 44,100 Hz), and feature-unit mute/volume if present.
- **Rate estimator:** CYCCNT per SOF filtered over 32 ms. The reference rate is a single constant, a rational number of cycles per sample (ADR 0012, D-OS-8 amendment).
- **IN packetiser:** a phase accumulator choosing 44 or 45 frames per packet, plus the ring-fill guard. A packet never contains a size other than 44 or 45.
- **OUT feedback:** Q10.14 in 3 bytes at FS and Q16.16 in 4 bytes at HS, with the ring-fill trim.
- **SPSC stem ring and return ring** (reuse `core` SPSC), with overrun dropping the oldest entry and counting it.
- **Interleave/deinterleave kernels:** scalar reference plus a SIMD variant (`__PKHBT`/`__SMLAD`-style on M4, portable fallback on the host), unit-tested against each other. The slot-mask saturating sum is a separate kernel.
- **Counters struct** for the future `diag usb` command.

**Rules:** the OS-001 rules block (flags, no heap, no `double`, SPDX headers, `-fno-rtti -fno-exceptions`), plus ADR 0011 declared alignment for every packet buffer. No libc allocation, bounded loops, and nothing virtual in the per-packet path.

**Done when:**
- Unit tests cover:
  - descriptor golden bytes per mode × speed × profile, with the validator passing every one and failing on hand-made broken descriptors;
  - the packetiser and feedback property test against a simulated host SOF at ±500 ppm with ±150 µs jitter over at least 1e6 packets (more if fast): sizes always 44 or 45, fill in band, zero under/overruns after start-up, feedback error below 50 ppm after convergence;
  - Q10.14 and Q16.16 byte packing;
  - interleavers, SIMD against scalar, at random lengths and alignments;
  - the slot-mask sum;
  - the overrun policy.
- The host build (ASan/UBSan) passes ctest with 0 warnings. The MK66 build has 0 warnings, with `usb` text/data/bss reported against the ADR estimate of 4–6 KB for the whole stack.
- The reviewer approves.

**Out of scope:** the MK66 USB0 driver (006b), engine stem taps (OS-005), UI screens (Config > USB, Routing F4), and any audio in the simulator (maintainer).

## 006b — MK66 USB0 device driver, STANDARD mode, CDC `diag` console (ADR 0012 slice S4, first part; ADR 0011 item 1)
Builds on OS-007d. The device enumerates in STANDARD mode (USB-MIDI + CDC `diag`, the 006a descriptors) and answers `diag` commands over a serial terminal.

- **Driver (`platform/mk66/usb/**`)**, written from the K66 reference manual (USB OTG FS chapter: BDT, ping-pong buffers, `CTL`, `ISTAT`, `ERRSTAT`, `ENDPTn`, `ADDR`, `USBTRC0`, clock recovery). The Teensy core `usb_dev.c` and TinyUSB are behaviour and conformance references only. Cite them; copy no code.
  - **Clock.** 48 MHz from IRC48M with USB clock recovery, or from the PLL through `SIM_CLKDIV2`, whichever is valid at 180 MHz HSRUN with the OS-007b clock tree. Cite the RM.
  - **Enumeration.** The pull-up is enabled only after the BDT and EP0 are ready. EP0 control state machine: standard requests (`GET_DESCRIPTOR`, `SET_ADDRESS` applied after the status stage, `SET_CONFIGURATION`, `GET_CONFIGURATION`, `GET_STATUS`, `SET/CLEAR_FEATURE` halt, `SET_INTERFACE`/`GET_INTERFACE`). DATA0/1 toggles. A ZLP when the reply is shorter than `wLength` and a multiple of 64. STALL on anything unsupported. Bus reset, suspend and resume.
  - **CDC ACM.** `SET_LINE_CODING`, `GET_LINE_CODING` and `SET_CONTROL_LINE_STATE` are stored and ignored. Bulk IN/OUT go through `core` SPSC rings. TX never blocks: when the host does not read, the oldest data is dropped and counted.
  - **USB-MIDI.** The endpoints enumerate, and 4-byte event packets go to and from SPSC rings with drop-and-count. The MIDI engine binding is OS-005.
  - **Interrupts.** The USB0 ISR runs at the ADR 0013 D1 priority. It only moves packets between the BDT and the rings, with bounded work per interrupt. Command parsing and execution run in the main loop.
  - **Pure parts** live in `usb`: setup-packet parsing and the control-transfer plan (request → reply slice, ZLP, STALL), the toggle and ping-pong bookkeeping, and the CDC line state. They are host-tested against a fake controller and get a QEMU suite.
- **`diag` console** (ADR 0011 item 1). A line shell that is pure and host-tested: a line editor with backspace and a bounded length, plus a dispatcher. Commands are actions with string names, not a second registry (ADR 0009 rev 1). This slice provides:
  - `help`;
  - `version` (build hash);
  - `stage`;
  - `cpu` and `mem` (the OS-005a formatters);
  - `crash` (the OS-007b record);
  - `usb` (the 006a counters plus the driver's counters);
  - `log` (a ring buffer that boot and drivers write to);
  - `input` (live events, as INPUT TEST does);
  - `reset` (`SYSRESETREQ` only).

  **Out of scope:** `update`, the bootloader pin and anything that writes `_fwinfo`. They stay maintainer gated (ADR 0011 item 3).
- **Identity.** Use the VID/PID header from 006a (the official firmware's pair, kept by the maintainer's decision (2026-10-16, confirmed 2026-10-18); no second PID, AUDIO is told apart by `bcdDevice`). The VID/PID question is decided; the descriptors for AUDIO mode are not exposed by the driver yet.
- **Image.** The default MK66 image enumerates in STANDARD mode. Report flash and RAM deltas against the ADR estimate of 4–6 KB for the whole stack. The images pass the static checks (flash config, contiguity, reproducibility).
- **Done when:**
  - Host, QEMU and MK66 ctest pass with 0 warnings.
  - The pure parts and the shell are covered: every standard request, ZLP cases, STALL paths, the toggle sequence, line editing, the command table, and TX drop policy.
  - The reviewer approves, with a hardware-safety angle (clock source at HSRUN, pull-up timing, VBUS handling, nothing touching the bootloader pin or `_fwinfo`) and a real-time angle (ISR cost, rings, priority versus audio and display DMA).
  - The hw checklist (`platform/mk66/README.md`) gains:
    - enumeration on Linux, macOS and Windows;
    - every `diag` command;
    - unplug and replug, suspend and resume;
    - CDC throughput;
    - no audio underruns while `diag` streams;
    - an `o3console.py` smoke test, if the host tool is added. That stdlib host tool is optional in this slice.

## 006c — Firmware update over USB: upload a `.ptf` to `Firmware/`, hand over to the bootloader (Level C)
Authority: the maintainer's firmware-delivery policy ("through the supported OS USB update path (OS-006c)"), which lifts the ADR 0011 item 3 gate for this path only. Behaviour reference: the hand-over observed on the unit and recorded in `docs/hw/tracker-hardware-notes.md` §3 (no code copied; how the official firmware starts its own update is an open research question).

**What the bootloader needs (all we know).** A `.ptf` (Intel HEX) in `Firmware/` on the SD card; `Firmware/_fwinfo` containing exactly the bare file name (no folder, no newline, no NUL; a stale `_fwinfo` is deleted first); then the bootloader request pin, Teensy pin 81 = **PTA1** (`board::kBootloaderRequest`) configured as a GPIO output (MUX 1, DSE, SRE) and driven **low**. On the unit this hand-over flashed the named image (`fw update`, 2026-10-12: `platform/mk66/README.md`, "What the bootloader needs"); that the bootloader then deletes `_fwinfo`, and how the pin and the file name were first learned, has no method recorded in this repository (open research question). The hidden reset button flashes the **first** `.ptf` of `Firmware/` in directory order: a broken candidate that lands first makes the button reflash it (recoverable only by moving the card). Nothing else is known; record what is not.

**Safety goals, in order.** (1) Never hand the bootloader an image that could lock the MCU or not boot: the flash configuration field and the image shape are checked on the device, on the received stream **and** again on the bytes read back from the card, and on the host before sending. (2) Never write outside `Firmware/` (OS-FWANY, maintainer decision 2026-10-13, lifted the "only a marked test card" half of this goal: any writable FAT card with a `Firmware/` folder takes the writer; OS-ANYCARD, 2026-10-17, removed the marker gate and its build option `O3OS_FW_REQUIRE_TEST_CARD` and made the host's `--expect-volume` optional: any SD card may be written). (3) A half-written file is never a `.ptf`. (4) The update is never started by a stray line. (5) The read-only guarantee of OS-007e stays true for everything except one narrow, separately testable writer.

### Scope
- **SD write, narrowly.** `sd::Cmd` gains **CMD24 WRITE_BLOCK** only (and CMD13 already exists for the busy/status poll; CMD25/ACMD23 not added). The SDHC policy gets a write data phase (DTDSEL 0, fill the FIFO at the write watermark, `PRSSTAT` DAT0 busy wait bounded in `sd_policy.h`, CRC-status error = failure, CMD13 after each block). Still forbidden and asserted absent by the 256-value test: ERASE (32/33/38), PROGRAM_CSD (27), SET/CLR_WRITE_PROT (28/29), LOCK_UNLOCK (42), GEN_CMD (56), CMD25. The `BlockDevice` gains `write`; a card whose write-protect switch is set, or whose CSD says permanently/temporarily write-protected, is refused for writes (read works as before).
- **FatFs** with `FF_FS_READONLY 0`, `disk_write` and `disk_ioctl(CTRL_SYNC)` in the glue. `hal::storage::Storage` (the read contract) is **unchanged**. A new, separate contract `hal::storage::FirmwareStore` (or equivalently named) is the only writer: `begin_upload(name)`, `append(bytes)`, `finish()` (sync, close, read-back verify, rename), `abort()`, `remove(name)`, `write_fwinfo(name)`, `remove_fwinfo()`, `list()` in directory order. Only one translation unit (the FatFs-backed implementation) may reference FatFs write-class symbols (`f_write f_sync f_unlink f_rename f_truncate f_mkdir f_expand f_setlabel f_mkfs f_chmod f_utime`); a test or `check_layers.py` rule enforces it (e.g. `nm` of the other objects).
- **Path guard (pure, host-tested exhaustively).** The writer accepts only: the temporary file `Firmware/_o3up.tmp`; `Firmware/_fwinfo`; and final names matching `[0-9A-Za-z][0-9A-Za-z._-]{0,47}` followed by `.ptf` (suffix in any case; no `/`, `\`, `..`, leading dot, control chars, spaces; the maintainer dropped the `PolyendTracker_` prefix requirement after the first unit run, so a candidate need not look like an official file). Create/rename refuse an existing target. `remove` accepts only the temp file and valid image names that do not look official (a name starting `Polyend`, any case, is protected unless it starts `PolyendTracker_0.`, our old dev namespace: versions Polyend never published), and refuses to remove the last `.ptf` of the folder. `Firmware/` must already exist (no `mkdir`). **Card:** no marker file is needed (OS-FWANY, OS-ANYCARD; the first design required `/.o3-test-card`), and `fw` reports the FAT volume serial (`XXXX-XXXX`) so a host that wants to can match an expected card.
- **Streaming Intel HEX validator (pure, host and QEMU tested).** Fed bytes in any chunking; same rules as `tools/check_image.py check`: record syntax and checksum, types 00/01/02/04 only (no start-address record), EOF last and nothing after it but line ends, no byte given twice, addresses ≤ `0x000FFFFF`, contiguous from 0 with no hole, `0x0..0x40F` present, **no check of the initial SP, the reset vector or the vector table** (OS-BUG-UPDFOREIGN, maintainer 2026-11-18: the device installs any well-formed image, the reset button is the way back; `check_image.py check --profile device` mirrors this, the release gate keeps them), **flash configuration `0x400..0x40F` exactly `ffffffffffffffffffffffffdef9ffff`, FSEC & 3 == 2** (its own copy of the constant, plus a `static_assert` against `flash_config.h`). It keeps O(1) state plus the first 0x410 bytes; it reports the image length and a CRC-32 of the file bytes. A cross-test runs it and `check_image.py` on the real built HEX and on the check_image self-test's mutations and requires identical verdicts.
- **Transfer protocol over the CDC `diag` console** (pure state machine, tested against a fake store and the real shell/pump):
  - `fw put <name> <bytes> <crc32hex>`: refuses (one `ERR <reason>` line) on a bad name, existing target, no `Firmware/`, size 0 or > 4 MiB, a load in progress, insufficient free space. Otherwise creates the temp file and prints `ready`. The receiver then **owns the terminal** (like the pager: no line editor, no echo) and consumes **exactly** `<bytes>` raw bytes of the HEX file. Allowed bytes are `:0-9A-Fa-f\r\n`; anything else (Ctrl-C included) aborts. It validates while it writes (buffered writes, a sector multiple), acks progress with `+<bytes>` at least every 4096 bytes (the host sends at most 4096 bytes past the last ack), and on the first error prints `ERR <reason> at <offset>`, deletes the temp file and discards the remaining declared bytes before returning to the prompt. An idle gap > 5 s aborts the same way. At the end: the validator's final checks, CRC match, sync/close, a **full read-back** of the temp file re-run through a fresh validator and CRC, then rename to `Firmware/<name>`; it prints `ok <name> <bytes> <crc32> image <len>` and `first-ptf <name>` (the reset-button target after this change).
  - `fw`: write path state (enabled, volume serial, write-protect), the `Firmware/` listing in directory order with sizes and which `.ptf` is first, the upload state, the last result.
  - `fw rm <name>`: per the path guard.
  - `fw arm <name> <crc32hex>`: re-reads and fully re-validates the file, checks the CRC, prints `armed <token>` (8 hex digits, unpredictable enough: DWT/UID/uptime mix; one use; expires after 30 s; cancelled by any other `fw` command or a reset).
  - `fw update <token>`: valid only for that token. Stops the transport, mutes the outputs, deletes a stale `_fwinfo`, writes it (the bare name), syncs, reads it back and compares, prints `update: handing over to the bootloader (<name>)`, lets the TX drain (≤ 40 ms, as `reset`), then configures PTA1 (gate PORTA clock if needed, PCR = MUX1|DSE|SRE, PCOR, PDDR) and drives it low. If the MCU is still running 5 s later, release PTA1 (input, MUX 0), delete `_fwinfo`, log and print `ERR bootloader did not take over` (USB keeps being serviced during the wait). PTA1 is touched **nowhere else**; a test asserts the only writer of `kBootloaderRequest`.
- **Host tool** `platform/mk66/tools/o3console.py`: `--put FILE.ptf [--name N]` runs the local `check_image.py check` first and refuses on failure, then `fw put` with chunked sending and ack waits, prints the device reply; `--update NAME --crc X --yes-update` runs `fw arm` + `fw update`, waits for the port to disappear and re-enumerate (timeout e.g. 120 s), then runs `version` and prints it (and compares with `--expect-version` if given); `--expect-volume XXXX-XXXX` (optional) makes put/update refuse a different card. Self-test extended against the pseudo-terminal (a fake device implementing the protocol, including an injected `ERR`).
- **Docs:** README section "Firmware update over USB (OS-006c)" (protocol, safety layers, cost, the reset-button ordering hazard, what is unknown about the bootloader), the `diag` table, the SD "read only" text corrected to "read only except the `Firmware/` writer", a hardware checklist FW-1..FW-n (first install of a 006c image still goes the old way, by card; then: put to a test name, `fw`, arm/update to a known-good image, the reboot, `version`; a refused write-protected card; a pulled cable mid-put; a card pulled mid-put; `_fwinfo` absent after a good update; the 5 s fallback cannot be provoked safely and stays unverified).

### Rules
OS-001 rules block; main loop only (no SD in an ISR), every wait bounded; no heap; 0 warnings; audio keeps running during `fw put` (writes are main-loop slices: at most one sector batch per pass, the display serviced). The reset button/recovery path is never affected by the image (the bootloader is separate). Measure and report flash/SRAM deltas (expected roughly 8–14 KB flash).

### Done when
- `tools/ci.sh host`, `qemu`, `mk66` exit 0 with 0 warnings; the image passes the static checks.
- Tests that matter: the HEX validator (every record defect, every byte of the flash config flipped, holes, SP/reset/vector cases, chunking 1..N, cross-check with `check_image.py`), the path guard (accept/refuse tables, fuzz-ish), the command allowlist (exactly 16 with CMD24, the forbidden list absent), the software card's write sequence (CMD24, busy, CRC-status error, write-protect refusal), FatFs writes on the FAT32 test image (create/append/rename/unlink/read back; the tree outside `Firmware/` byte-identical; the last-`.ptf` and existing-target refusals), the transfer state machine (good upload, each error path, discard of the declared remainder, idle timeout, disk full, card pulled, CRC mismatch, read-back mismatch), arm/token (expiry, reuse, wrong name), the `_fwinfo` bytes, and the host tool self-test.
- Reviewer approves with the angles: flash-config/MCU-lock safety, SD write correctness (and, since OS-FWANY, that no path outside `Firmware/` is ever written on any card), the PTA1 hand-off, real-time impact.

**Out of scope:** MSC/MTP file transfer, `get` of files, a general SD writer, saving projects, any change to the bootloader, SWD.

### 006c as built
Everything in the scope above is implemented and tested except what only a unit can show (README of `platform/mk66`, "Firmware update over USB" and checklist FW-1..FW-10). Where it differs from the text above:
- **Placement.** The write contract is `platform/mk66/update/firmware_store.h` (`FirmwareStore`), not a `hal` header; `hal::storage::Storage` is untouched. The FatFs-backed store, the `fw` service and the HEX validator live in `platform_mk66_pure` (host and QEMU run them); the image-only parts are the SDHC write phase (`storage/src/sdhc_bus.cpp`), the `FwHost` (`update/src/fw_host_mk66.cpp`: stop the transport, mute, drain, PTA1, entropy) and the `fw` binding in `usb/src/console.cpp`. The raw byte stream needed a small addition to the shell (`Shell::start_raw` / `end_raw`).
- **Write-class symbols.** One source may name them (`update/src/fat_firmware_store.cpp`), enforced by `tools/check_layers.py` rule 6 (with the glue's `disk_write` / `disk_ioctl` in `storage/src/fat_storage.cpp` only); rule 7 does the same for `kBootloaderRequest` (only `board.h` and `fw_host_mk66.cpp`). No `nm` check.
- **HEX validator.** Same verdicts as `check_image.py` on 160 corpus files and, by command, on the real image and 30 mutations, with one deliberate difference: data records must be in ascending, consecutive address order (O(1) state), so shuffled records that check_image accepts are refused (`HexOrder`). The head it keeps is the first 0x410 bytes (the 116 vectors lie below the flash configuration field). The SRAM_L code window constant is its own copy, `static_assert`ed against `mem_map.h`.
- **Space.** "Insufficient free space" is detected at `begin_upload` by reserving the clusters (a seek past the end of the file in write mode), because R0.15a's `f_getfree` scans the whole FAT on every call.
- **exFAT** volumes are refused for writing. **Write protect:** the slot's contact is not read, only the CSD's PERM/TMP_WRITE_PROTECT bits and a `SdBus::write_protect_switch()` that is false on the Tracker; a card with its tab set is not protected by this code (FW-3 asks the unit).
- **Protocol details.** Acknowledgements every 2 KiB (>= 4096 was the bound); one `f_write` of a 2 KiB buffer per main-loop pass; after an idle abort the receiver does not wait again to discard; `fw arm` needs an enabled write path; `fw update` also compares the card's volume serial and the file's size with those at arm, refuses while a project loads and re-validates the whole file (CRC included) once more before `_fwinfo`; the 5 s fallback runs on the main loop's clock (`FwService::poll`), independent of the terminal; a closed terminal (DTR drop) abandons an upload and deletes its temporary file. A failed `CMD24` is not retried.
- **o3console.** As specified, plus exit codes 3..7 documented in its header. The self-test is the existing ctest `mk66_o3console_selftest`.
- **Measured cost** (clean `ci.sh mk66`, default image, against the previous build): see the README ("Cost"); larger than the 8-14 KB estimate (FatFs write code is 6.1 KB of it).

