# OS-BUG-SELFFLASH — a unit that changed to the official firmware with nobody at it

Status: specified 2026-11-06 (drafted by an agent; nothing decided by the maintainer yet), not implemented · Level C for any change to the image (hardware safety: the update path and a pin the bootloader reads), Level A for the evidence slices (read-only, no code) · Backlog row OS-BUG-SELFFLASH (`docs/roadmap/BACKLOG.md:106`, found 2026-11-06, open research question) · Facts from `dev` at c875105e and from the row's own incident note · The cause is **unknown**: this spec does not claim one. It lists what is known, the leads, what would tell them apart and the cheapest order to find out, so the next unattended run is both safe and informative. No agent touches the unit: every step on it is a tester or maintainer session.

## Maintainer decisions (to take)

| # | Question | Options | Recommended default | Why |
|---|---|---|---|---|
| Q1 | What sits first in `Firmware/` while any run goes unattended | a) the last known-good **Track3r** image first (and only), the official file in `Firmware/old/` · b) the image under test only · c) as the incident card was (an official file first) | a | Whatever starts the bootloader without a name flashes the first `.ptf` (F6, F7). With a the worst case is a flash to a build of this OS, visible as a changed `version`, never the official firmware and never a silent loss of the test image; it is also the existing recovery rule (`platform/mk66/README.md:1545`). b risks reflashing a broken candidate; c is the incident |
| Q2 | Closed: W3a merged on 2026-11-06 (994fc158) after a clean 10-minute unattended run; the bug stays open apart from it | none | |
| Q3 | Whether the image may change a pin at boot on suspicion (set PTA1 to a plain input with pull-up before anything else) | a) no change until S1 or S2 points at the pin · b) do it now as a guard | a | The pin is the update request line (F1); a guard changes a line the bootloader reads (hardware safety, Level C) and would hide the cause rather than find it; S1 reads the pin without changing it |
| Q4 | Who runs S1 and S2 on the unit | a) the tester, with the host tools the repository already has (`o3console.py`, `o3hil.py`), reporting to a `build-images/<version>/REPORT.md` · b) the maintainer by hand | a | The row's own "cheap check, read only" is five `peek` lines; S2 is a repeat of a run the tester already does |

## 1. Goal

Find what made a running unit start its bootloader with no request from the OS, or rule the OS out; until then make an unattended run unable to leave the unit on the official firmware. Success is a named cause with the observation that proves it, or two unattended repeats with the evidence of S1 and S2 collected and nothing seen, and the rule of Q1 in force.

## 2. Facts

| # | Fact | Where | Label |
|---|---|---|---|
| F1 | The only code that drives the bootloader request pin is `Mk66FwHost` (PTA1, Teensy pin 81: GPIO output, driven low from its first instant); `fw_service.cpp` calls it once, after `_fwinfo` is written, synced and read back; the layer check forbids any other source from naming the pin. After 5 s without a reset the pin is released as an input with its reset pad function (JTAG_TDI, pull-up, `PCR` 0x703: from the reference manual's table, never read on a unit) and `_fwinfo` is deleted | `platform/mk66/include/o3os/platform/mk66/board.h:134-136`; `platform/mk66/update/src/fw_host_mk66.cpp:3-12`, `:32-37`; `platform/mk66/update/src/fw_service.cpp:725-742` (hand-over), `:441-452` (the 5 s fallback); `platform/mk66/update/include/o3os/platform/mk66/update/fw_service.h:70` | code fact |
| F2 | Two ways reach that call: the console `fw update <token>` (needs an `fw arm` first, token valid 30 s) and Config, INFO, `F8`, the update dialog (open, UPDATE, YES, then INSTALL: five keys; every step re-checks that no recording, edit or console `fw` operation is running; a name over 30 characters is never armed). The HIL key injection stops while an update is armed | `app/src/firmware_actions.cpp:151-170`, `:173-225`; `hal/include/o3os/hal/firmware.h:40`; `platform/mk66/usb/include/o3os/platform/mk66/usb/hil_inject.h:12-13`; `platform/mk66/README.md:1209-1210` | code fact |
| F3 | The watchdog is switched off at reset and never enabled; a reset the image causes itself is `SYSRESETREQ` through one function, after the fault handlers have written the crash record (a 128-byte `.noinit` block that survives that reset; "survives a watchdog or lockup reset" is an open hardware check). The boot records the reset cause (`RCM_SRS0/1`) in `BootInfo::reset_sources`, and `diag stage` prints it | `platform/mk66/src/startup.cpp:105-111`; `platform/mk66/src/crash_store.cpp:96-109`; `platform/mk66/include/o3os/platform/mk66/crash_record.h:5-15`; `platform/mk66/usb/src/console.cpp:204` | code fact |
| F4 | The console's `peek` reads aligned words in a fixed list of windows, which already holds SIM, PORTA..E (`0x40049000`-`0x4004DFFF`: PTA1's `PCR1` is `0x40049004`), GPIO (`0x400FF000`-`0x400FFFFF`: GPIOA `PDIR` is `0x400FF010`) and the System Control Space (`0xE000E000`-`0xE000EFFF`: DHCSR `0xE000EDF0`, DEMCR `0xE000EDFC`). The reset control module (RCM) is not in the list | `platform/mk66/usb/include/o3os/platform/mk66/usb/mem_access.h:27-45`; register addresses from the K66 reference manual (not machine-readable here) | code fact for the windows; the addresses are from the manual [unverified] |
| F5 | The incident (the row): the bench image `v1.1.1-w3a.1` (`feat/OS-CEWTENV-W3a` 7e698540) was 4 min 13 s into an unattended governor run of Dense8w12x; it left USB at 15:48:33 and came back 38 s later as the official firmware (`bcdDevice` 2.77). No session sent update, reset or `disk` commands then; the maintainer was away. The first `.ptf` of `Firmware/` on the card was an official file, and no `_fwinfo` was left. Watchdog, console `fw update` and the boot pin mux were ruled out by a read-only pass the same day (not repeated here) | `docs/roadmap/BACKLOG.md:106` | row fact (second-hand) |
| F6 | The hidden reset button flashes the **first** `.ptf` of `Firmware/` in directory order (with several files it flashed `PolyendTracker_0.0.4.ptf`); it is how the unit was recovered from every hung image. What the bootloader does with a button held at a reset is not known | `docs/hw/tracker-hardware-notes.md:29`, `:72` | OBSERVED (the button); the rest open |
| F7 | An update armed with a file name of 33 characters or more installed the **first** `.ptf` instead of the named file, with no error and no crash (2 of 2); 29 and 30 characters installed the named one. So the bootloader has a path "no usable name, flash the first file" that is reached without the reset button | `docs/hw/tracker-hardware-notes.md:28` | OBSERVED (the rule is an open research question) |
| F8 | That the bootloader deletes `_fwinfo` after a flash "is not observed here" (`FW-8` of the update checklist expects it absent and has never been run to the end on a unit). So "no `_fwinfo` was left" fits an update request that the bootloader consumed as well as a flash that never had one | `platform/mk66/README.md:1198-1200`, `:1556`; `docs/hw/tracker-hardware-notes.md:26` | code fact (the unknown is stated there) |
| F9 | The time a normal `fw update` takes from the pin to the new `version` is not recorded anywhere, so whether the incident's 38 s is a bootloader flash time is unknown | `docs/hw/tracker-hardware-notes.md:26` (no figure); the README checklist FW-8 asks for it | open research question |

## 3. The leads and what separates them

All unverified. "Fits" means the lead can produce F5; a discriminator is an observation that supports or kills it.

| Lead | Fits F5 because | Against it | Discriminator | Cost |
|---|---|---|---|---|
| L1 An update request the OS made without being asked (the dialog or the console) | `_fwinfo` absent fits a consumed request (F8) | No session sent one; the dialog needs five keys and F2's checks; an official file installed, which a request that names one of our images would not give, unless through F7's name path | the card's FAT: a deleted `_fwinfo` entry with a creation time at 15:4x; the unit's own log of `update:` lines (`diag log`) is gone with the reboot | S1 (card image, read only) |
| L2 PTA1 low at some reset: the line is JTAG_TDI with a pull-up (F1) and nothing in the OS drives it; a floating or loaded line, or a probe, could read low | the pin is the documented request line | the pull-up; the OS never configures it | `peek` PORTA `PCR1` (expect `0x703`) and GPIOA `PDIR` bit 1 (expect 1) at idle and under the governor run, on the same image | S1 (five reads) |
| L3 The bootloader flashes the first `.ptf` after a reset whose cause it dislikes (a watchdog or core-lockup reset) | F6/F7 show a first-file path that needs no name; the OS has a fault path that ends in `SYSRESETREQ` (F3) | the OS reset is `SYSRESETREQ` and this OS has crashed and recovered many times (crash records read back after a normal reboot, `docs/hw/tracker-hardware-notes.md:31`); the watchdog is off (F3) | a lockup reset cannot be told after the fact; S2 logs `reset_sources` on every boot and the uptime so a repeat that comes back on ours shows the cause the OS saw | S2 |
| L4 A fault with the core halted (C_DEBUGEN or a vector catch left on) and then an outside reset | the row's leading path | a halted core does not come back by itself; needs the outside reset | `peek` DHCSR (bit 0 C_DEBUGEN) and DEMCR (VC_ bits, TRCENA) at idle: a read of DHCSR clears its sticky bits, read it last | S1 |
| L5 The board or the bootloader (a brown-out or USB-supply glitch resets the chip while the bootloader is waiting for something; the bootloader then takes its recovery path) | nothing in the OS is needed | not testable from the OS; the unit ran the same bench for hours before | S2 on a second supply (the powered hub vs the host port) and the same image on a second unit if one exists; a log of the host's USB over-current events | S2 |

## 4. Slices

| # | Slice | Size | Level | Flash [est] | Depends on |
|---|---|---|---|---|---|
| S1 | Read-only evidence: the card's FAT forensics, five `peek` reads on the unit, the update timing | S | A | 0 | Q4 |
| S2 | An instrumented unattended repeat with the rule of Q1 | S | A/B | 0 (host tooling; at most a boot-line change in the image, see below) | Q1, S1 |
| S3 | The fix, once a lead is named | S-M | C | decided by the cause | S1 and S2 |

**S1** (S, Level A: reads only; nothing on the card or the unit is changed). (a) If the incident card has not been written since: make a read-only raw image of it (a card mounted from a reader, identified as an SD card before touching it, `dd` from the device node, never a write) and list its deleted directory entries in `Firmware/` with a FAT tool: a `_fwinfo` entry with a create or write time inside the incident minute decides L1. If the card was written (the row says files were moved into `Firmware/old/`), say so and skip. (b) Boot the unit on the same bench image, then `o3console.py --port <port> "peek 40049004 1"`, `"peek 400FF010 1"`, `"peek E000EDFC 1"`, `"peek E000EDF0 1"` (last), `"peek 40048004 1"` (SIM_SOPT2; address from the manual, a reserved word would fault, so one word), idle, and again a minute into the governor run; write the words into the report. (c) With the first `.ptf` rule of Q1 a in place, run one normal `fw update` of that image (the README's FW-8) and record the time from the pin to the new `version` (F9) and whether `_fwinfo` is there afterwards (F8). Acceptance: the words read, the FAT result or "card overwritten", the two timings and F8's answer in `REPORT.md`; every lead of section 3 marked supported, killed or open.

**S2** (S, Level A, host tooling; B only if the image is touched). The same governor run, unattended, with: Q1 a on the card (first `.ptf` = the known-good Track3r image); a host watcher that logs once a second the USB state of the unit (the descriptor's `bcdDevice`, which tells ours from the official firmware at once), `diag stage` (uptime and `reset_sources`) on each reconnect, and the listing of `Firmware/` before and after; two runs of at least 30 minutes [est]. Optional, image side, Level B: print `reset_sources` and the DHCSR word in the boot log line so that a recovery to ours is readable without `diag stage` (files: `platform/mk66/src/boot.cpp`, `platform/mk66/usb/src/diag_text.cpp`, one host text test; flash a few tens of bytes). Acceptance: two clean runs, or a drop with the three logs; either way the report says which leads that kills.

**S3** (Level C, not specified further). The decision table once a lead is supported:

| Cause found | Candidate change | Level |
|---|---|---|
| L2 (PTA1 low) | at boot set PTA1 to a GPIO input with the pull-up on (`PCR` MUX 1, PE, PS) and keep it so; `fw update` still drives it; pin-ownership table and `check_layers.py` rule 7 updated | C |
| L3 (bad reset cause) | find and remove the path to a core lockup (a fault inside a fault handler); a host/QEMU test that a fault ends in `SYSRESETREQ`, never in lockup | C |
| L4 (debug state) | clear DHCSR/DEMCR state at boot is not possible from the core; document the bench rule (power-cycle after a probe) | A |
| L1 | a refusal in the path that made the request; a test that no key sequence of the HIL tools reaches INSTALL | C |
| L5 | a bench rule (supply), nothing in the image | A |

## 5. Assumptions (decided here; each local, testable and reversible)

- A-1 The incident is real and the row's facts are true; they are quoted second-hand (F5), never re-measured here.
- A-2 No step of this spec writes to the unit, flashes it or arms an update; S1 (c) runs the documented `fw update` path of the README on a tester-owned unit, after `fw-gate-check`, with a known-good first `.ptf`.
- A-3 Reading `DHCSR` through `peek` is acceptable (it clears sticky status bits of a debugger that is not attached); it is read last.
- A-4 The thresholds "two runs of 30 minutes" are estimates [est]; the incident came at 4 min 13 s.

## 6. Risks

- R1 **A second self-flash during S2**: Q1 a makes it land on a Track3r image; the report records the version either way.
- R2 **Card overwritten**: S1 (a) may find nothing; the other evidence stays valid.
- R3 **`peek` on a hung unit**: the window list is checked and refused outside it (F4); a read inside a window can still fault on a reserved word, which the record would show.
- R4 **The fix hides the cause**: Q3 a keeps the image unchanged until a lead is supported.

## 7. Evidence labels

- F1-F4, F8: **code fact**; F5: **row fact** (second-hand); F6, F7: **OBSERVED** on the unit (earlier sessions); register addresses: **from the manual, unverified**.
- Every lead of section 3 and F9: **open research question**.
- The rule of Q1 and the run lengths: **new Community OS behaviour** (a process rule for this repository, not a product feature).

## 8. State for implementer

- **S1.** Start with: `platform/mk66/README.md:1196-1212`, `:1542-1558` (the bootloader's known behaviour and the update checklist); `platform/mk66/usb/include/o3os/platform/mk66/usb/mem_access.h:27-45`; `platform/mk66/tools/o3console.py` (`peek`, `fw`); `docs/hw/tracker-hardware-notes.md:22-34`. Rules: no agent touches the unit or any card; the tester does, and identifies an SD card as one before reading it (CLAUDE.md); the card image is never committed; the report lists words, not hostnames or serials.
- **S2.** Start with: the governor run's generator (`platform/mk66/tools/hil/gen_wt_arms.py`, `gen_hil.py`) and `platform/mk66/tools/o3hil.py`; `BootInfo::reset_sources` `platform/mk66/include/o3os/platform/mk66/platform.h:37` and `platform/mk66/src/crash_store.cpp:96-99`; `diag stage` `platform/mk66/usb/src/console.cpp:204`. Rules: `bcdDevice` tells the firmwares apart (official 2.77, ours per mode, `docs/specs/OS-USBDISK.md:33`); one `.ptf` first rule of Q1 before the run starts; the run is Level B with a flash/SRAM delta if the boot line changes (the host text test `platform/mk66/tests/mk66_usb_text_test.cpp`).
- **S3.** Start with: `platform/mk66/update/src/fw_host_mk66.cpp`, `platform/mk66/include/o3os/platform/mk66/board.h:124-136`, `tools/check_layers.py` rule 7. Rules: Level C: risk review plus one independent reviewer, `tools/ci.sh qemu` and mk66 before a candidate image, hardware verification by the tester; the decision table above is a menu, not a plan.
- Report per slice: the words read, the logs, which leads moved, and the first-`.ptf` state before and after every run.
