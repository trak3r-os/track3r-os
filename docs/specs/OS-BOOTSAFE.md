# OS-BOOTSAFE: a crash-loop guard and a safe mode

**PARKED by the maintainer 2026-11-05.** Kept as a record; nothing here is scheduled.

Level C (the boot path, hardware safety). Approved by the maintainer 2026-11-05 (IDEA triage); it comes after OS-GATE-BOOTCRASH (QUEUED). Design only: nothing is implemented. All of it is **new Community OS behaviour**. No parity is claimed, and the official firmware's crash behaviour was not looked at. Facts come from the tree at dev a6fc0b57 and from the OS-BUG-COSMBOOT tester reports of 2026-10-31.

## Decisions for the maintainer

Only product behaviour is asked here. The technical details are decided below as assumptions (A1 to A12).

**Decided by the maintainer 2026-11-05: every recommended default (Q1 a, Q2 a, Q3 a, Q4 a with INSTALL as slice S3, Q5 a, Q6 a).** The latent bug found here (the boot copy of the crash record is cleared before `make_modules()` reads it) is the IDEA row OS-BUG-CRASHQUAR, proposed to come before S1b.

| # | Question | Options | Recommended default | Why |
|---|---|---|---|---|
| 1 | What counts as a crash loop? | (a) 3 crashes in a row, each before the unit has run 10 s past its boot load; (b) 2 in a row; (c) 5 in a row | (a) 3 | One crash can be bad luck. Three in a row with no good run between is a pattern. One crash cycle took about 11 s on the unit (cosm.1, F6), so safe mode comes about 35 s after power-on. With 5 it would take a minute. |
| 2 | What does the screen show in safe mode? | (a) One text page: SAFE MODE, why (the last crash in one line), the build, the USB and card state, and the keys; (b) a dark screen, with the LED code and the console only; (c) the normal app with a SAFE banner | (a) | A user without a computer must see what happened and what to press. (c) would run the code that may be crashing. If the display itself is what crashes, the ladder (3.3) falls back to (b) by itself. |
| 3 | How does the user leave safe mode? | (a) A key on the safe screen, `safe exit` on the console, or Power off and on; (b) Power off and on only; (c) the key or the console only (Power off and on stays in safe mode; only pulling the cable clears it) | (a) | Every way a user would try works. If the crash is still there, the unit comes back after 3 more crashes, so leaving is never a risk. |
| 4 | What does the safe screen offer besides starting normally? | (a) START WITHOUT PROJECT (a normal start that does not open the last project) and INSTALL FIRMWARE (a `.ptf` from `Firmware/`, through the existing check and install); (b) START WITHOUT PROJECT only; (c) nothing: the rest is done from a computer | (a), with INSTALL as its own slice (S3) | A crash at boot most often comes from data (a project) or from an image (a bad update). These two options cover both cases without a computer or the reset button. |
| 5 | Can the user enter safe mode on purpose? | (a) The boot chord Shift + F1 + F6, and `safe enter` on the console; (b) the console only; (c) no | (a) | ADR 0002 already promises "a button held at boot gives a safe mode" (F15). It is the way to START WITHOUT PROJECT, or to reinstall, on a unit that does not loop. |
| 6 | What happens when safe mode itself keeps crashing? | (a) After safe mode and the minimal mode have each crashed once, stop: no more restarts, and the LED blinks code 15 until power is removed; (b) go on restarting, as today | (a) | A loop that cannot be broken gains nothing by going on. Pulling the cable starts again from a normal boot. |

## Assumptions (decided here: technical, local, reversible)

- **A1. Where the count lives.** A 20-byte guard record goes in the reserved bytes of the existing 128-byte crash block (`.noinit` at `0x20000000`; 56 bytes are free there, F14), at offset 72. It has five words: magic `O3BG`, an image id, a packed word (streak u8, flags u8, drill_left u8, drill_point u8), the CRC of the last crash record it counted, and a CRC-32 of the first 16 bytes (the existing `crash::crc32`). Nothing moves: the block keeps its size and its address. It is RAM state across a reset, not a file. Because of the image id, a guard written by another image is never read, so there is no compatibility burden across versions.
- **A2. Single writer.** The guard is written only by the reset sequence (before interrupts) and by the main loop (the healthy point, the `safe` command, the safe screen's keys, the power-off). The fault handlers and `panic()` are not changed and never touch it. A write cut by a crash fails its CRC, so the guard reads as fresh: a failure always errs toward a normal boot.
- **A3. Fresh start.** The guard starts again from zero (streak 0, no flags, no drill) when any of these holds: the reset sources include POR, LVD or PIN (a power-on, a brown-out, or the reset pin); the magic or the CRC is wrong; the image id differs from this image's. The image id is FNV-1a of `O3OS_BUILD_LABEL` and the image's end address, a link symbol.
- **A4. What is counted.** At a boot, a crash record that validates, whose CRC differs from `last_crc`, and whose run had not reached the healthy point, adds 1 to the streak. Every exception the record can carry counts (HardFault, MemManage, BusFault, UsageFault, NMI, an interrupt with no handler, and a software panic). So does a LOCKUP reset with no record (a fault inside the reporter). Nothing else counts: a clean SYSRESETREQ (`reset`, the Power wake, the USB DISK key restart, a hand-over that falls back) leaves the streak as it is. A crash after the healthy point counts 0, and the streak goes back to 0. The streak saturates at 255.
- **A5. The healthy point.** It is reached in a normal run when the boot project load has ended (or none started) and the main loop has then run 10 s (`kHealthyAfterMs`). There the guard is written with streak 0 and the `run_healthy` flag set. A boot clears `run_healthy` after it has made its decision. INPUT TEST, the USB DISK boot and a start without project reach it the same way. A safe rung never reaches it.
- **A6. The decision point.** The verdict is computed in `Reset_Handler` right after `capture_boot_record()` (step 4, before the clocks), so a crash in a static constructor (step 8, before `main`) is counted too. The verdict is Normal, Safe, Minimal or Halt, from the streak with T = 3 (Q1): below T Normal, T Safe, T+1 Minimal, T+2 or more Halt. Halt is acted on after SysTick starts (step 6): `boot::fail(blink::kBootHalt)`, with nothing else started. `main` acts on Safe and Minimal.
- **A7. The order in the safe rungs.** USB first (STANDARD: the console and USB MIDI, whatever the settings say, which are not read). Then the card (Safe only). Then the display and its screen (Safe only). So a hang later in the bring-up still leaves a console up. Every wait on that path is already bounded.
- **A8. The LED.** A new code 14 (`kSafeMode`) shows in place of the heartbeat in Safe and Minimal. A new code 15 (`kBootHalt`) shows forever in Halt. Codes 2 to 13 are taken (F12). The blocking crash-seen code 6 still plays at every boot that finds a record, as today.
- **A9. The crash stamp.** The safe rungs run with module id `0xD6` (`kSafeModule`), and the drill with `0xD5` (`kGuardDrillModule`). Both are in the 0xD1..0xD4 family of ids no loader hands out (`main.cpp:56-62`), so a record says where it died.
- **A10. The safe screen** uses the boot screen's path (`ui/src/boot_screen.cpp`: its own word buffer, a `Renderer` with the boot theme) after `ui::upload_assets()`, so the simulator can render it for a golden. It is redrawn only when a fact on it changes (at most every 100 ms). It reads its keys through the console's tap, so `key` injection works for HIL.
- **A11. Not here.** Safe mode never relaxes a firmware check (OS-GATE-BOOTCRASH's `NotPadded`, OS-UPDPAD's pad offer). It is not woken by the watchdog (still off, F11) and does not change the fault handlers.
- **A12. Flash budget.** All slices together stay within 8 KB of flash (about 61 KB is free, F13), and S1a with S1b within 4 KB. The SRAM_U cost is 0 B: the guard sits inside the existing block, and the screen's word buffer goes on the stack, as the boot screen's does, or in SDRAM.

## 1. Goal

Today a boot crash restarts the unit, and the next boot crashes again, for ever. There is no USB and no screen, so the only way out is the hidden reset button with a good `.ptf` first in `Firmware/`, which means taking the card to a computer (OS-BUG-COSMBOOT, F6). The guard counts crashes across resets. After a crash loop it boots into a safe mode that starts the USB console (and, in the first rung, a text screen and the card) and nothing that can be the cause: no project, no audio, no modules, no settings. From there the user can start normally, start without the project, or reinstall firmware, all without the reset button.

## 2. Facts

| # | Fact | Where | Label |
|---|---|---|---|
| F1 | Every fault handler and `panic()` write a 64-byte `CrashRecord` (magic, version, count, exception, CFSR, PC, LR, a module/stage context, uptime, CRC-32) to the 128-byte `.noinit` block at `0x20000000`, then reset with SYSRESETREQ. | `crash_store.cpp:144-170`, `crash_record.h:46-72`, `mem_map.h:70-71` | new Community OS behaviour; survival across SYSRESETREQ observed on the unit (cosmdiag report 2026-10-31: reset 0x0400, the record read) |
| F2 | `count` is meant as "a boot counter for quarantine", but `main` reports the record and clears it at every boot before anything else (`main.cpp:192-198`). On the unit it is 1, except for a crash before `main`. | `crash_record.h:51`, `main.cpp:192-198` | code reading |
| F3 | **Latent:** `clear_crash_record()` also drops the boot copy (`crash_store.cpp:117-122`), so `boot_crash_record()` is null when `make_modules()` reads it (`app_main.cpp:811-813`, reached through `start_app` → `make_document` → `make_sound` → `make_modules`, `:1696, :1076, :848`). The ADR 0002 boot quarantine of a crashed module therefore never fires, and `diag stage` says "crash record at boot: no" after a crash. This contradicts `platform.h:46-49` ("stays valid for the whole run"). | as cited | code reading; the `stage` line was seen on the unit (cosmdiag report) |
| F4 | Reset sequence: early pins and stage Reset, then the C runtime, then the reset cause and the record captured (`startup.cpp:144-146`), then clocks, interrupts and SysTick, then SDRAM, then constructors, then `main` (`:148-172`). A crash in a constructor happens before `main`. | `startup.cpp:101-176` | code reading |
| F5 | Boot order in `main`: the crash code 6 is played twice and blocks (8 s; `blink.h:47-49`). Then the display (module 0xD2, `main.cpp:204-238`), audio (`:252-263`), the card, settings and the last project (module 0xD4, `:269-313`), USB (module 0xD3, `:321-327`), and the loop (`:355`). | `main.cpp` | code reading |
| F6 | The cosm.1 crash loop: LED code 6 repeating, no USB, and the screen left on the bootloader's picture. The recovery needed the card in a computer and the hidden reset button. The crash was a BusFault from `display::Chip::init`, and the record said "stage Running, module 0" at 2.4 s, so the step stamp did not name the display (why is an open research question). | BACKLOG OS-BUG-COSMBOOT; `docs/hw/tracker-hardware-notes.md:31`; tester reports `build-images/v1.0.1-cosm.1/`, `v1.0.1-cosmdiag.1/` (untracked) | observed on the unit |
| F7 | A diagnostic image that went headless after a crash record brought the console up after one crash: `main` at 2454 ms, display skipped, USB configured at 17754 ms (6.4 s of that is the display-failure LED code). | cosmdiag report (untracked) | observed on the unit, diagnostic image, not merged |
| F8 | Two boot modes already exist. INPUT TEST (Shift + F1 + F8) and USB DISK (Shift + F1 + F7) are read in `appimg::init` after `start_app` (`app_main.cpp:2658-2712`, `boot_chord.h:17-27`). USB DISK reads no settings and no project, and is left by a key that resets (`app_main.cpp:2878-2887`). | as cited | new Community OS behaviour |
| F9 | The console update path needs no screen: `fw put`, `fw arm` and `fw update` are polled from `console::service()` (`console.cpp:1551`), and its quiesce calls `appimg::stop_transport()` and `audio_device().mute()` (`fw_host_mk66.cpp:55-60`). The hidden reset button flashes the first `.ptf` of `Firmware/`. | `platform/mk66/README.md` "Firmware update over USB"; hardware notes `:29` | observed on the unit (OS-006c, the cosmdiag install) |
| F10 | Power off ends in `boot::reset_chip()` (a dry restart, or the VLPR wake), which is a SYSRESETREQ, not a POR. The noinit SRAM is kept, so a Power off and on clears nothing unless the code clears it. In a crash loop Power does nothing (no main loop); pulling the cable is the way off. | `power_host_mk66.cpp:293-300`; `docs/specs/OS-POWER.md:206` | code reading |
| F11 | Reset sources come from RCM SRS0/SRS1: POR `0x0080`, WDOG `0x0020`, PIN `0x0040`, LOCKUP `0x0200`, SW `0x0400`. The watchdog is off, so a hang never resets. Whether SRAM survives a lockup or a watchdog reset is not known. | `platform.h:33-43`; `main.cpp:240-245`; README `:297` | code reading; open research question |
| F12 | LED codes 2 to 13 are in use. `boot::fail()` plays one code for ever with nothing else running. | `blink.h:19-38`; `boot.cpp:168-173` | code reading |
| F13 | v1.1.0 used 94.14 % of flash, so about 61 KB is free. | `docs/STATUS.md:7` | measured by the build |
| F14 | The crash block holds the record, the context pair and 56 reserved bytes. | `crash_store.cpp:26-32` | code reading |
| F15 | ADR 0002 promises a boot counter and "holding a button at boot gives a safe mode with no modules"; neither is built. ADR 0012 says a crash loop in the card mount gets no console: the fix is to remove the card. | `docs/adr/0002-sd-plugins.md:48`; `docs/adr/0012-usb-audio.md:220` | design text |
| F16 | The shell has one gate hook, already used to restrict commands during a shutdown (`kPowerReads`, `power_gate`). | `command_table.h:69`; `console.cpp:1466, 1482` | code reading |
| F17 | HIL scenarios may not send `reset`, `fw` or `poke` (they change the unit). QEMU (mps2-an386) runs the unit suites; a fault ends the run, and nothing boots the MK66 path. | `o3hil.py:320`; `platform/qemu/src/startup.cpp` | code reading |

## 3. Design

### 3.1 What counts as a crash loop

A crash loop is T = 3 consecutive counted boots (Q1, A4): each boot found a crash record (or a LOCKUP reset) from a run that never reached the healthy point (A5). Any of these ends the streak: a healthy run, a POR or reset-pin start, Power off, `safe exit`, the safe screen's START keys, or a different image.

### 3.2 The guard record and what clears it

The guard record is described in A1 to A3. It is cleared, in full or in part, by these events:

| Event | Effect on the guard |
|---|---|
| POR, LVD, PIN reset; a bad CRC; another image id | fresh (all zero) |
| A boot with a counted crash (A4) | streak + 1, `last_crc` = the record's CRC |
| The healthy point (A5) | streak 0, `run_healthy` set |
| Power off, at the start of `enter_off()` (F10), in every run | fresh |
| `safe exit`, safe screen F1 START | streak 0, then reset |
| Safe screen F2 START WITHOUT PROJECT | streak 0, `no_project_once` set, then reset; the next boot uses the flag and clears it |
| `safe enter`, the chord Shift + F1 + F6 (Q5) | streak = T, `run_healthy` cleared, then reset |
| `reset`, the USB DISK restart, a hand-over that falls back | unchanged |

The noinit crash record is still cleared by `main` after it has been reported, as today. The guard has already counted it by its CRC.

### 3.3 The rungs: what each starts

| Part | Normal | Safe (streak T) | Minimal (T+1) | Halt (T+2 or more) |
|---|---|---|---|---|
| Reset sequence, SDRAM test, constructors | yes | yes | yes | stops after SysTick (A6) |
| USB | the settings' layout | STANDARD, first (A7) | STANDARD | no |
| Console | every command | allowlist (3.7) | allowlist | no |
| SD card | mount, settings, last project | mount only: no settings, no project, no card watch for modules | not mounted at boot; `sd mount` mounts it | no |
| Display | app | the safe screen (A10) | not touched | not touched |
| Input | buttons and pads | buttons only (the pads stay off) | no | no |
| Audio (codec, I2S, engine), MIDI DIN, modules | yes | no; the amplifier key stays LOW from `early_pins` | no | no |
| Power switch | yes | yes (Power off clears the guard) | yes | no (pull the cable) |
| LED | heartbeat or codes | 14 | 14 | 15 |

Minimal exists for the cosm.1 case: the display bring-up itself crashed, and the step stamp did not say so (F6). The ladder needs no stamp. Safe crashes in its display, and the next boot is Minimal, which never touches the display or the card. That is the cosmdiag behaviour (F7), now general. Minimal does not mount the card, because a crash in the mount would take its console away too (F15).

### 3.4 What the screen and the LED show

The safe screen is one page in the boot screen's theme. The wording is final in S2, and every field has a worst-case length test:

```
SAFE MODE
The unit restarted 3 times after a crash. It started without the project, the sound and the modules.
Last crash: BusFault 2.4 s after the start (pc 04ED8352, lr 00014B89)
Build: v1.1.0 (a3fe987526ec)
USB: console ready | not connected        Card: 3 firmware files | no card
Power off and on also starts normally.
[F1 START] [F2 NO PROJ] ... [F8 FIRMWARE]        (F8 from S3)
```

The LED shows code 14 in Safe and Minimal, and code 15 in Halt (A8). The console's `safe` command and `state` (`screen=SAFE level=safe streak=3 ...`) say the same over USB.

### 3.5 Leaving safe mode (Q3)

There are four ways out, and all of them start a normal boot:

- F1 START on the screen;
- `safe exit` on the console;
- Power off and on (the guard is cleared in `enter_off()`);
- pulling the cable (POR).

F2 starts normally without opening the last project or the Workspace (`main.cpp:296-303` skipped once); settings are read as usual. If the crash is still there, the unit comes back to safe mode after T crashes. From Halt, only pulling the cable (or the reset button) works.

### 3.6 Reinstalling firmware from safe mode

- **Computer, Safe and Minimal:** `o3console.py --put FILE`, then `--update NAME --crc ... --yes-update`. This is the existing path (F9). In Minimal, type `sd mount` first. FwHost's `quiesce()` must be a no-op where it needs the app or the sequencer (to check in S1b).
- **On the unit, Safe (S3, Q4):** F8 FIRMWARE lists `Firmware/*.ptf` through the existing `hal::firmware::FirmwareSink` (`update::device::sink()`): `list_begin` / `list_next`, `begin_check` / `poll_check`, `pad`, `install`. It uses the same validator, the same `NotPadded` rule and the same PAD offer as Config's Update dialog, and the same 30 s token.
- **Unchanged:** the reset button and the official update menu.
- **After any install:** the new image has another image id, so it starts with a fresh guard (A3). The same image installed again keeps its guard.

### 3.7 The console in safe mode

- **The safe gate.** A gate like `power_gate` (F16) allows `help`, `version`, `stage`, `crash`, `log`, `mem`, `sd`, `ls`, `fw`, `reset`, `power`, `state`, `key`, `shot` (if it works without the app; otherwise refused) and `safe`. Every other command answers `ERR safe mode: safe exit to leave`.
- **`safe`** prints the level, the streak of T, the healthy flag, the armed drill and the last counted crash, in every run.
- **`safe exit`** and **`safe enter`** work as in 3.2.
- **`safe drill <1..8> <main|display|usb>`** and **`safe drill off`** arm a test hook. For the next n boots that reach the chosen point, `drill_left` is decremented and stored before a UDF with module `0xD5`:
  - `main`: normal runs only, at the top of `main`;
  - `display`: every run that brings the display up;
  - `usb`: every run that starts USB.

  The drill cannot loop for ever, whatever the guard does. Each armed crash is spent before it fires, and a POR, `safe exit` or Power off disarms it.
- **o3hil** refuses `safe` with arguments in scenarios, as it refuses `reset` and `fw` (F17).

### 3.8 How it is tested

- **Host (ASan, UBSan) and QEMU (the same suite on mps2-an386).** These run the pure `BootGuard`, in `platform_mk66_pure`:
  - the table of 3.2 row by row;
  - the ladder from 0 to 255;
  - the same record counted once;
  - LOCKUP with and without a record;
  - a crash after the healthy point;
  - the image id;
  - every single-bit flip of the 20 bytes reads as fresh, never as Halt;
  - 10,000 random boot sequences with no crash record never leave Normal;
  - drill accounting.

  There are also the console gate (each command in and out of the allowlist), the safe screen's word count and text fit (like `boot_screen_test`), and a simulator golden of the safe screen (`sim/src/screens.cpp`, like `draw_boot`).
- **Not on QEMU.** The real reset loop cannot run there (F17). Whether QEMU keeps RAM across SYSRESETREQ, which would allow one, is an open research question.
- **Unit, with the normal gated image.** Nothing unusual is flashed. The tester uses o3console, because scenarios may not crash the unit:
  - `safe drill 3 main` gives 3 crashes, then Safe;
  - `safe drill 4 display` gives 3 crashes, Safe crashing in its display, then Minimal;
  - `safe drill 5 usb` gives Halt.

  S1a ships the counting with no action, so the first unit runs prove the count while the boot cannot change. A failure there costs at most n crashes. Halt and the POR clear need a cable pull, which is the maintainer's hands; they are optional, and the host proves the logic.
- **Every image.** `boot_version.json` gains `{"cmd": "safe", "expect": "level normal"}`, a cheap check against false positives on every image the tester installs.

### 3.9 Fit with OS-GATE-BOOTCRASH

OS-GATE-BOOTCRASH stops the known cause of a boot crash (an unpadded image) at every door of the update path. OS-BOOTSAFE is the net for the causes nobody has found yet. They fit in three ways:

- Safe mode reinstalls only through those same doors, unchanged.
- The guard adds no `.noinit` symbol: it lives at a fixed offset inside `g_block`, so a future check_image rule that pins `g_block` at `0x20000000` (that row's open idea) covers it.
- That row's other idea, a host boot-to-first-frame test, would also run the healthy point.

## 4. Slices

All slices are Level C. Each needs a risk review plus one independent reviewer, the scoped host tests, `ci.sh qemu` and `ci.sh mk66`, and a gated image for the tester. QA rows go in `docs/qa/catalogue/global.md`, "Safe mode (OS-BOOTSAFE)"; renumber if another branch takes these ids first.

| Slice | Size | What | Files | Acceptance and tests | Unit (HIL) | QA rows | Flash / SRAM [est] |
|---|---|---|---|---|---|---|---|
| **S1a: count, observe only** | S | The `BootGuard` (pure), its words in the crash block, the decision in `Reset_Handler`, the healthy point, the clear in `enter_off()`, `safe` (status), `safe drill`, the log lines. The verdict is logged, never acted on. | new `boot_guard.h/.cpp`, `mk66_boot_guard_test.cpp`; `crash_store.cpp`, `startup.cpp`, `main.cpp`, `power_host_mk66.cpp`, `console.cpp`, `command_table.h`, `o3hil.py`, `boot_version.json`, README "Faults and the crash record" | the host and QEMU tests of 3.8; no change to any boot path (the log diff is the only difference) | `safe` reads normal and 0; `safe drill 1 main` gives 1 crash, `safe` reads 1, then 0 after the healthy point; `safe drill 3 main` gives 3 crashes and `safe` reads "verdict safe (observe only)"; Power off and on gives 0 | QA-GLB-174, 175, 176 | +1.0 to 1.5 KB / 0 B |
| **S1b: act, headless** | S | Halt in `Reset_Handler`; `main` hands Safe and Minimal to a headless `safe_main`: USB STANDARD, the card in Safe, the console gate, LED 14 and 15, `safe exit`, `safe enter`, Power | new `safe_main.cpp`; `startup.cpp`, `main.cpp`, `blink.h` (+ `mk66_blink_irq_test`), `console.cpp`, README (LED codes, reset sequence, a Safe mode section) | the gate test; `fw` with no app (quiesce null-safe); LED distinctness | `safe drill 3 main` gives the safe rung headless: console up, `fw put` and `fw arm` of a padded image work, `ls` works, `load` is refused; `safe exit` gives a normal boot; Power off and on gives normal; time from power-on to the console measured | QA-GLB-177, 178, 179, 180 (Halt, hands, optional) | +1.5 to 2 KB / 0 B |
| **S2: the safe screen** | S-M | The screen (3.4), the buttons, F1 START and F2 START WITHOUT PROJECT, `no_project_once` in `main`, the chord Shift + F1 + F6 (Q5), `state` and `shot` in safe mode | new `ui/.../safe_screen.h`, `ui/src/safe_screen.cpp`, `tests/safe_screen_test.cpp`; `safe_main.cpp`, `app_main.cpp` (the chord), `main.cpp`, `sim/src/screens.cpp`, `docs/ui/ui-map.md`, `docs/manual/install.md` (a "Safe mode" section), ADR 0002 (an amendment line) | the screen test and the sim golden; the chord's pure test in `mk66_boot_chord_test` (F6 next to F7 and F8, never two chords at once) | `safe enter` shows the screen (`shot`); `key f1` gives normal; `key f2` gives normal with no project (`state`); `safe drill 4 display` gives Minimal; the chord by hand (maintainer, optional) | QA-GLB-181, 182, 183, 184 | +2 to 3 KB / 0 B |
| **S3: INSTALL FIRMWARE** | S | F8 FIRMWARE: the list, check, PAD and install through `FirmwareSink` | `safe_main.cpp`, `safe_screen.cpp`, `tests/safe_screen_test.cpp`, `docs/manual/install.md` | the list and check states rendered (fake sink); the install never runs in a test | the list and IMAGE OK by HIL; the INSTALL press is the maintainer's (key injection is refused while a check waits) | QA-GLB-185, 186 | +1.5 to 2 KB / 0 B |

## 5. Risks

- **Safe mode itself crashes.** The ladder (3.3) handles it: Safe crashes, so Minimal (USB only); Minimal crashes, so Halt, which stops the loop. The safe code is small and runs with module `0xD6`, so its record says so. Minimal touches neither the display nor the card. Residual risk: a crash in the USB bring-up or in a constructor leaves Halt and the LED only, which is still better than today's endless loop.
- **A false positive on a normal unit.** It needs T counted crashes with no healthy run, no POR and no Power off between them. A unit that does not crash has no record. Garbage in SRAM is stopped by the magic, the CRC and the POR rule. A guard left by another image is stopped by the image id. The checks are the host's random-sequence test and `safe` in every image's smoke run. A real but rare crash at boot three times in a row is a true positive; F1 START leaves.
- **SRAM across other resets.** Survival across a lockup or a watchdog reset is unknown (F11). If SRAM is lost there, the guard reads as fresh: today's behaviour.
- **The bootloader.** It may keep SRAM across a flash; the image id makes that harmless. Its own reset source after a flash is not known.
- **Flash.** About 61 KB is free; this item takes at most 8 KB (A12). Stop and report if S1a with S1b goes over 4 KB.
- **The codec is not started.** After a crash the codec keeps its registers, and nothing mutes it in a safe rung (the amplifier key is LOW). That is the same as today's boot after an audio failure. Whether the headphone output is quiet then is an open hardware check (MK66-3 style, listening).
- **The drill in release images.** A console user can crash the unit on purpose. It is bounded (n ≤ 8, spent before it fires) and cleared by POR, `safe exit` and Power off.
- **The time to safe mode.** At about 11 s per crash cycle (F6, the 8 s LED code included), safe mode comes about 35 s after power-on. This is measured in S1b.
- **Reviving the module quarantine** (F3), if it is fixed first, changes what a module crash does at the next boot. That is the documented ADR 0002 behaviour, but it has never run on the unit.

## 6. Findings recorded, not fixed here

- **F3:** the boot copy of the crash record is dropped before the module system reads it, and `diag stage` says no record. The smallest fix is to stop `clear_crash_record()` from clearing `g_boot_info.crash_record_found`. Recommended as its own row (OS-BUG-CRASHQUAR, S, Level C, before S1b). It is not part of these slices, and the guard does not need it.
- **F6:** cosm.1's record named module 0 for a crash in the display bring-up. That is an open research question about where the stamps are set.

## 7. State for implementer

- **Branch:** `feat/OS-BOOTSAFE-s1a` (then one branch per slice). Level C: a risk review and one independent reviewer per slice. Do not start S1b before S1a's unit check has passed.
- **Read first:**
  - `crash_record.h` (the whole header);
  - `crash_store.cpp`;
  - `startup.cpp:100-176`;
  - `main.cpp:182-238` and `:269-327`;
  - `platform.h:33-54`;
  - `boot_chord.h`;
  - `app_main.cpp:2658-2712` (the chords), `:2878-2887` (the DISK restart), `:3033-3040` (`power_pass`);
  - `power_host_mk66.cpp:285-300`;
  - `console.cpp:1440-1490` (the gate);
  - `command_table.h`;
  - `ui/src/boot_screen.cpp`;
  - `hal/include/o3os/hal/firmware.h` (S3).
- **Steps for S1a:**
  1. `BootGuard` (pure) and its test.
  2. The words in `NoinitBlock`, with a `static_assert` that the reserved space is still at least 0.
  3. The decision call in `Reset_Handler` after `capture_boot_record()`.
  4. The healthy point in `main`'s loop, through `appimg::project_loading()`.
  5. The clear in `enter_off()`.
  6. `safe` and `safe drill`.
  7. The log lines, and `safe` in `boot_version.json`.
- **Rules:**
  - the fault handlers and `panic()` are not changed;
  - nothing new is written to flash or to the card;
  - no check of the update path is relaxed;
  - the guard is written only where A2 says;
  - every new boot wait is bounded;
  - never flash or touch a card yourself.
- **Size [est]:** about 1.0 to 1.5 KB of flash for S1a, SRAM_U 0 B, CPU per audio block 0.
- **Stop and report** if:
  - the guard does not fit the reserved bytes;
  - the decision cannot run before the constructors;
  - `fw` needs the app in a safe rung;
  - S1a with S1b goes over 4 KB of flash.
