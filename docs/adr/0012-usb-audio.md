# ADR 0012 — USB audio: multichannel stems out, stereo in

Status: **Accepted with amendments** (maintainer-delegated design decision, 2026-10-11). See "Amendments at acceptance" at the end; they override the text above them.

**Inputs:** radar card `usb-multichannel-audio` (prior art, host facts, first estimates), ADR 0003 (stems are buses, not tracks), ADR 0005 (GPLv3 core), ADR 0007 (chunked project), ADR 0009 (settings are parameters), ADR 0011 (`diag` CDC, USB identity), `docs/design/product-ux.md` (Routing F4 STEMS, Config USB), `rt-optimize`.
Tags: **[fact]** sourced or read in the repo; **[est]** arithmetic or judgement, a bench item for the next `hw-session`; **[inf]** inference. STATUS.md item 14 asked for exactly this ADR before any code.

## Recommendation
1. **Full-speed USB Audio Class 2.0 (UAC2), 16-bit, one isochronous IN stream of up to 11 channels, default profile 10 = 8 mono pre-pan stems + master L/R, plus a 2-channel OUT return with explicit feedback.** One descriptor generator takes a `Speed` parameter, so a high-speed port is a later driver, not a redesign.
2. **Declare 44,100 Hz and run at the true engine rate (44,117.65 Hz); no resampling.** The true rate travels in the IN packet cadence (44/45 frames per ms) and the OUT feedback value. D-OS-8 should retune the engine to exactly 44,100 Hz; this design works either way.
3. **Own USB stack in `os/`:** pure C++17 (descriptors, class requests, packetiser, feedback, rings) tested on the host, plus an MK66 USB0 driver written from the reference manual. TinyUSB and the Teensy core serve as conformance references only. USB mode is a boot-time choice, **STANDARD** (today) or **AUDIO**, both with `diag`; STANDARD stays the default.

## Context
- **Port [fact].** These are facts of this OS's early test images, which ran on the Teensy core (PJRC `usb_dev.c`), not of the official firmware: the image drives the K66 full-speed OTG controller `USB0` (12 Mbit/s; `usb_dev.c`, priority 112). The K66 also has a USBHS controller with an integrated HS PHY (https://www.nxp.com/products/K66_180); on Teensy 3.6 it is a 5-pin host header (https://www.pjrc.com/store/teensy36.html), and PJRC says it has "no working software support" (https://forum.pjrc.com/index.php?threads/multichannel-usb-audio-with-teensy-3-6.41370/).
  - NXP's SDK ships HS device mode on this chip (EHCI, FRDM-K66F VCOM example, https://community.nxp.com/t5/Kinetis-Microcontrollers/USB-driver-for-MK66-MCU-USB-HS-device-mode/m-p/604788), so HS device mode is technically possible.
  - TinyUSB lists Kinetis KL/K32L as full-speed only, "Highspeed ❌", and no K6x row (https://raw.githubusercontent.com/hathach/tinyusb/master/README.rst).
- **Tracker OG board: UNKNOWN.** `docs/hw/tracker-hardware-notes.md` records no USB routing. The vendor documents one USB-C port (card: documented, not verified). Whether USB1 D+/D- reach any connector or pad is not known. The USB-C is almost certainly on USB0, because that is the controller the test images drive [inf]. "USB host pins" is already on the `hw-session` list (STATUS.md). **HS is therefore not on the critical path.**
- **Engine clock [fact].** MCLK = 180 MHz x 16/255 from the system clock (`MCLK_SRC 0`), so one sample is exactly 4,080 CPU cycles and the rate is 44,117.647 Hz (+400 ppm, +0.69 cent, 24 ms per minute against 44,100 Hz). The SOF interrupt (1 kHz) and DWT CYCCNT already exist, so the device can measure its clock against the host's SOF with no extra hardware.
- **Host drivers [fact].**
  - Windows `usbaudio2.sys` (UAC2, Windows 10 1703+): asynchronous IN and OUT; async OUT only with an explicit feedback endpoint, no implicit feedback; IN packet size within +/-1 audio slot of nominal; more than 8 channels not supported in shared mode; "limited support" for one clock shared by several endpoints; a MIDI function in a UAC2 device is enumerated separately (https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/usb-2-0-audio-drivers).
  - Windows UAC1 `usbaudio.sys` lists only synchronous and adaptive endpoints (https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/usb-audio-class-system-driver--usbaudio-sys-); the card records a September 2026 Windows 11 regression for UAC1 multichannel.
  - Apple TN3190 (https://developer.apple.com/tutorials/data/documentation/technotes/tn3190-usb-audio-device-design-considerations.md): ADC1 is 1023 B/ms, 12 spatial locations and "ambiguous" clocking; ADC2 clock sources are fully supported; FS bus carries 1,500 B/ms; async IN tolerance is +/-1 sample per ms; explicit feedback is 3 bytes at FS and 4 at HS; HS ADC2 carries up to 3,072 B per 125 us.
  - XMOS ships FS UAC2 as a supported mode (https://www.xmos.com/documentation/XM-012296-UG/html/doc/rst/opt_audio_class.html). Linux `snd-usb-audio` covers UAC1/UAC2 (card).

## Decision 1 — port and speed
| Option | Pros | Cons |
|---|---|---|
| A. FS UAC1 (PJRC stock model) | smallest delta if we kept the PJRC code unchanged | this OS is a new tree, so there is nothing to reuse; Windows UAC1 driver lists no async; Sept 2026 multichannel regression; ambiguous clocking on macOS; 12 channel locations; cannot run at HS (Apple: HS ADC1 violates the spec) |
| **B. FS UAC2 (recommended)** | one clock-source model; Windows documents async + explicit feedback; Apple supports ADC2 fully; same descriptors serve HS later; per-channel names | larger descriptors (about +100 B [est]); we write class requests ourselves; Windows "shared clock" caveat for IN+OUT (test one vs two clock entities) |
| C. HS UAC2 on USBHS | 85 channels at 16-bit, 56 at 24-bit per 1,024 B microframe; 30 stems at 24-bit = 540 B [est] | pin routing UNKNOWN; needs an EHCI device driver nobody here has written; second connector or hardware change may be needed |
| D. FS UAC2 with two IN streams (more than 11 channels) | more channels | 12 channels + return + feedback is about 87% of the 1,500 B frame [est], within 3 points of the 90% periodic share hosts reserve (USB 2.0 §5.6.4, from memory) and leaves almost nothing for MIDI and CDC; Windows probably lists each stream as a separate device [inf]; rejected |

**Channel arithmetic [est]**, 44,117.65 Hz, worst-case packet 45 frames (cadence is 44/45), limit 1,023 B per FS endpoint; HS packet 6 frames per 125 us microframe, limit 1,024 B.
| Layout | Ch | FS 16-bit B/ms | FS 24-bit B/ms | HS 24-bit B/microframe |
|---|---|---|---|---|
| Master only | 2 | 180 | 270 | 36 |
| 8 mono stems | 8 | 720 | 1,080 **no** | 144 |
| 8 stems + master L/R (default) | 10 | 900 | 1,350 **no** | 180 |
| largest FS 16-bit layout | 11 | 990 | 1,485 **no** | 198 |
| 12 stems + master L/R | 14 | 1,260 **no** | 1,890 **no** | 252 |
| 12 stereo stems + master + 2 stereo sends | 30 | 2,700 **no** | 4,050 **no** | 540 |
- **Largest layouts:** FS 16-bit 11 channels (5 stereo pairs + 1 mono); FS 24-bit 7 channels (3 pairs); HS 24-bit 56 channels (28 pairs).
- **Default bus load:** 900 (IN) + 180 (OUT) + 3 (feedback) + 3 x 9 B overhead = 1,110 B, 74% of 1,500 B. The rest (about 390 B/ms) is all that MIDI, CDC and file transfer can use while streaming.
- **16-bit only** (the engine is int16 end to end). 24-bit does not fit more than 7 channels and carries no extra information.
- **Composite.** AUDIO descriptor set = UAC2 function + USB MIDI + CDC `diag` (ADR 0011). Endpoint numbers [est]: audio 2 (data IN/OUT share a number as in PJRC's `USB_EVERYTHING`, plus feedback IN), CDC 2, MIDI 1, so 5 of 15, which the descriptor test asserts.
  - **MSC/MTP: not in AUDIO mode.** File transfer would crawl in the 26% left; it lives in STANDARD (MIDI + CDC `diag` + file transfer as decided elsewhere).
  - **Why two sets, not one composite:** a mis-enumerating audio function (Windows binding cache, driver quirks) must never cost MIDI, `diag` or file access, and the user opts in. Same pattern as the Digitakt (card).
  - **Recovery:** AUDIO reverts to STANDARD after two failed or crashed boots (ADR 0002 boot counter). SD-bootloader recovery is unaffected.

## Decision 2 — the odd clock
| Option | Pros | Cons |
|---|---|---|
| **A. Declare 44,100; run at the true rate; async both ways (recommended)** | DAW sees a standard rate; zero DSP; hosts already tolerate +/-1 sample per ms; real rate carried by cadence and feedback | the DAW's timeline is the device clock, so against a wall-clock reference stems are 400 ppm fast (24 ms per minute, 0.05 BPM at 120) [est]; inaudible alone, visible only against an external clock |
| B. Declare 44,118 Hz | label is honest | a nonstandard session rate DAWs will not offer [inf]; Windows shared mode resamples anyway |
| C. Resample to 44,100 in the device | label exact | 529k samples/s x 12-20 cycles = about 3-6% CPU [est]; a 4-tap interpolator colours the audio; still needs async feedback against SOF |
| D. Retune engine to exactly 44,100 Hz (D-OS-8) | exact label; ends the export "drop one sample per ~2,600" kludge | maintainer decision outside this ADR; changes every pitch by 0.04% (the official firmware's own rate is not known: `docs/hw/tracker-hardware-notes.md` section 5); needs hardware proof |
- **Descriptors.** One internal fixed Clock Source at 44,100 Hz (GET RANGE reports one discrete rate); the host never changes it. Sessions at 48 kHz are the host's resampling job.
- **IN (device to host), free-running.** A phase accumulator fed by the measured SOF rate (CYCCNT per SOF, filtered over 32 ms) chooses 44 or 45 frames per ms. A ring-fill guard adds or removes one frame only if the ring leaves its band. The packet size never leaves {44, 45}, inside the +/-1 slot rule. This replaces PJRC's fixed 8x44+45 pattern, which is 6.5 samples/s short (card).
- **OUT (return), explicit feedback.** Feedback = filtered (cycles per SOF / 4,080) x 2^14, Q10.14, 3 bytes. The nominal value is 722,824 = 0x0B0788. A small ring-fill trim removes the start-up offset. The reference rate is one build constant (cycles per sample as a rational number), so D-OS-8 changes one line.
- **Retune arithmetic for D-OS-8 [est, verify in the K66 RM and on a counter].** `I2S_MDR` has an 8-bit FRACT and a 12-bit DIVIDE. 180 MHz x 196/3125 = 11,289,600 Hz = 256 x 44,100 exactly, and both values fit. The divider alternates between 15 and 16 input cycles, as 16/255 does today, so jitter should be unchanged.
- **Drift.** The IN rate comes from the device clock, so host crystal error is the host's problem. OUT is closed-loop. Counters in `diag usb`: IN size histogram, OUT ring min/max fill, under/overruns, feedback min/max.

## Decision 3 — stems design
- **Stem slots.** The USB stream has N channel slots (N from the profile). Each slot sums a bitmask of sources: T1..T12 mono taps, Master L, Master R, Send A return, Send B return. One source means a pointer and no arithmetic; several mean a saturating int16 sum (`__QADD16`), cheap against the 128-sample block.
- **Profiles (device setting `usb.profile`).**
  - `10` default: T1..T8 + Master L/R.
  - `8` compat, Windows shared-mode safe: T1..T6 + Master L/R.
  - `11`: T1..T9 + Master L/R.
  - `2`: master only.
  - `4S` (optional): 4 stereo post-pan stems + master.
  - Anything beyond 11 channels is refused by the Config UI, not truncated.
- **12 tracks, 11 channels.** Default mapping is T1..T8 to S1..S8; later audio tracks (and External audio tracks) are heard in the master, and the user can group them many-to-one in Routing F4 STEMS (for example S8 = T8+T9+T10). MIDI tracks have no audio and no slot. Track count never depends on the USB limit (ADR 0003).
- **Tap point.** Mono, pre-pan, after instrument, track FX and track level. One `StemTap` definition feeds both this stream and the SD stem export. Where the official firmware's export taps is an open research question (the release notes we have do not say: PAR-087), so USB stems follow whatever the SD export is decided to be (maintainer question).
- **UI.** `Config > USB`: mode (STANDARD/AUDIO, applied on re-attach by soft re-enumeration, reboot as fallback), profile, measured rate, counters. `Routing > F4 STEMS`: the slot matrix (USB column of the routing matrix in `product-ux.md`). Existing RMS meters double as stem meters.
- **Settings.** `usb.mode` and `usb.profile` are device-scope parameters. `stem_map` is a new project chunk with default-fill: a missing chunk loads the factory profile, unknown chunks are preserved (ADR 0007). It needs a spec entry and a round-trip test; no existing chunk changes.
- **Zero-copy path.**
```
engine block k (128 frames, int16 planar, alignas(4)) --block pointers--> StemRing (SPSC, depth 3)
  --USB ISR, every 1 ms: gather <= 45 frames x N ch, interleave straight into the BDT-owned packet buffer-->
  USB0 bus-master DMA --> host
```
  - One copy only (planar to interleaved, which the host format requires). No SDRAM, no staging ring.
  - Packet buffers `alignas(4)` + `static_assert` (ADR 0011 lesson 1), in SRAM_U so USB DMA should not contend with SRAM_L instruction fetch [inf: separate crossbar slave ports; bench it].
  - The tap is enabled only while the host has the stream's alternate setting 1 selected, so an idle device pays nothing.
  - Overrun drops the oldest block and counts it; the audio ISR never waits for USB.
- **Budget [est; card arithmetic carried over, to be benched].** Interleave 10 ch 0.74% + return 0.15% + ISR 0.42% = **about 1.3% of 180 MHz (1.0-1.8%)**. Worst ISR slice about 7.5 us at priority 112, against the 1.45 ms DMA half period. SRAM about 2 KB for packets and BDT + 5-8 KB of block lifetime in the engine pool (10 ch x 2-3 blocks x 256 B). Flash about 4-6 KB (UAC2 descriptors and requests + packetiser + feedback). Real-time rules hold: no allocation, no SD, bounded loops. Bench lines `BENCH usb_interleave`, `BENCH usb_isr`.

## Decision 4 — stereo input
- **Descriptor.** A 2-channel 16-bit OUT stream (180 B per packet), asynchronous with the explicit feedback endpoint of Decision 2. The nominal rate is the same clock source.
- **Path.** USB ISR writes interleaved frames into a ring (about 512 frames x 4 B = 2 KB [est], target fill about 2 ms). The engine's block pull deinterleaves into a planar stereo source named **USB RETURN** in the routing matrix, so it reaches MAIN, CUE and REC like Line L/R. Underrun yields silence and a count; overrun drops oldest. No SDRAM, no allocation.
- **Uses.** Sampler/recorder source ("Sampling over USB" in Tracker+ 1.1, card); optional monitor path that bypasses effects (matrix column); ADR 0011 item 5 (automated A/B capture) reuses the same plumbing.
- **Not for live monitoring through a DAW:** block + ring + host buffering is several milliseconds [est].

## Decision 5 — stack, licences, test levels
| Option | Pros | Cons |
|---|---|---|
| Teensy core `usb_dev.c` + `usb_audio.cpp` as-is | enumerates on this board today | UAC1 only; C-macro descriptors with no test seam; fixed packet pattern; PJRC clause 2 (below) |
| TinyUSB (MIT) | UAC1/UAC2, CDC, MIDI, MSC classes; actively maintained | no K6x port, Kinetis is FS-only and KL/K32L-only in its table; `tud_task` plus soft FIFOs add copies; flash and RAM unknown [est 15-25 KB]; C callback API in the real-time path |
| **Own (recommended)** | host and QEMU testable; speed-agnostic; about 4-6 KB; no licence debt; the same library serves HS later | EP0/BDT/ISR bring-up (about 600 lines [est]) and class-conformance risk, mitigated by the references and the host matrix |
- **Licence.** MIT code (TinyUSB, alex6679's UAC2 fork, the PJRC files' MIT-style text) can sit in a GPL-3.0-or-later core with notices in `THIRD_PARTY.md` (ADR 0005). LUFA is AVR-only and not applicable. PJRC's extra clause 2 (equivalent PJRC boards must be listed if the software is built into a system offering a list of target devices) is conditional and not triggered by a one-board firmware. Whether it is a GPLv3 "further restriction" is a legal question we avoid: the USB0 driver is written from the K66 reference manual, as `eve` was from the FT81X guide (ADR 0011 amendment); PJRC and TinyUSB are behaviour references. Bytes copied from either, such as descriptor layouts, keep their notices.
- **V1 host unit (doctest, ASan/UBSan).**
  - Descriptor golden bytes for every mode x speed x profile, plus a structural validator: lengths, `wTotalLength`, interface and endpoint numbering, IAD, `wMaxPacketSize` >= channels x bytes x 45 and <= 1,023, alt 0 with no bandwidth, at most 15 endpoint numbers.
  - Packetiser and feedback property tests against a simulated host SOF at +/-500 ppm with +/-150 us ISR jitter over 1e7 packets: sizes in {44, 45}, fill in band, zero under/overruns, feedback error below 50 ppm after convergence, Q10.14 and Q16.16 byte packing.
  - SIMD interleave and deinterleave against scalar references (random lengths, alignments); slot-mask saturating sum; settings and `stem_map` round trip.
- **V3 QEMU `mps2-an386`:** the same units on Cortex-M4F, covering the `__SMLAD`/`__PKHBT` paths. QEMU has no USB0.
- **V4 simulator:** UI only. Scenarios and goldens cover Config > USB and Routing F4. There is no audio in the simulator (maintainer, 2026-10-11).
- **V5 hardware only:** host class-driver acceptance, isochronous timing, ISR cost.

## Consequences
- STATUS.md item 14 becomes slices S1-S6 below. After acceptance, update the radar card status, `product-ux.md` (the full-speed ceiling is 11 channels, not 10) and STATUS.md. This ADR touches no other file.
- Nothing ships to the default (STANDARD) behaviour. The new USB identity needs a distinct PID for AUDIO mode and the unique serial of ADR 0011 item 6 (Windows keys driver bindings on both); the VID/PID is the official firmware's pair, kept by the maintainer's decision (2026-10-16, confirmed 2026-10-18), publication included (amendment 3); AUDIO mode is told apart by `bcdDevice`, with no second PID.
- Not decided here: 24-bit, 48 kHz sessions, an ASIO driver, more than 11 channels at FS.

## Slices
- **S1 (host, V1)** `usb` descriptor builder: UAC2 + MIDI + CDC, modes, profiles, validator, goldens.
- **S2 (host, V1/V3)** packetiser, rate estimator, feedback, rings, interleavers with benches.
- **S3 (device engine + host V1/V4)** stem slots and profiles in the engine (needs the OS-005 mixer), settings and the `stem_map` chunk, and UI for Config > USB and Routing F4. The slot-mask sum and the interleavers are unit-tested on the host; there is no simulator audio.
- **S4 (target)** `platform/mk66` USB0 driver: CDC `diag` first (needed by ADR 0011), then MIDI, then audio.
- **S5 (`hw-session`)** the list below. **S6 (conditional)** HS: only if item 1 finds USB1 routed; same descriptors with `Speed::High`, plus an EHCI device driver.

## Validation for the `hw-session`
1. **Board:** trace USB-C D+/D- and VBUS/ID nets; find any USB1 (HS) routing, pad or header; photograph. Decides whether S6 exists.
2. **Real rate:** frequency counter on LRCLK, or CYCCNT per 1,000 SOFs (expect 4,080 cycles per sample). If D-OS-8 retunes, test FRACT/DIVIDE 196/3125 for lock and SGTL5000 noise.
3. **Hosts:** macOS, Windows 10, Windows 11 (with and without KB5129195), Linux; at least Reaper plus two more DAWs. Check:
   - channels and names shown, and a 10-channel stream in Windows shared mode versus the 8-channel profile;
   - one versus two Clock Source entities (Windows "shared clock" caveat);
   - the MIDI function next to UAC2;
   - Windows driver-cache behaviour when switching modes.
4. **Soak:** 30 minutes at 10 channels + return on a laptop port and a powered hub (the vendor's documentation warns about computer USB power: documented, not verified; card): no clicks; packet histogram {44, 45}; feedback and fill counters flat.
5. **Cost:** DWT cycles for the interleave and for the worst `usb_isr` (iso IN done, OUT, feedback, SOF together); I2S DMA ISR jitter and MIDI timing with the stream open; free SRAM; pool high-water.
6. **Timebase:** record 10 minutes of a 120 BPM click into a DAW; measure drift against the grid (expect 24 ms per minute until D-OS-8, zero after).
7. **Return:** the sampler records from USB RETURN with 44.1 kHz and 48 kHz host sessions; round-trip latency; no drift over 10 minutes.
8. **Modes:** soft re-enumeration STANDARD to AUDIO and back without reboot; CDC `diag` and MIDI alive in both; two-failed-boot revert works.
9. **If USB1 is routed:** HS device bring-up (EHCI) and an HS enumeration with the same descriptors.

## Open questions for the maintainer
1. Does the official firmware's stem export tap before or after pan and level (an open research question), and may USB stems copy it (Decision 3)?
2. D-OS-8: retune to exactly 44,100 Hz? This ADR is neutral; the retune is the cleaner end state.
3. Default profile `10` (needs a non-shared-mode host on Windows) or `8`? Recommend `10` behind the opt-in AUDIO mode, with `8` one tap away; revisit after S5 item 3.
4. ~~A distinct PID for AUDIO mode, and the replacement of the provisional VID 0x16D0 / PID 0x0E9F by the project's own pid.codes PID.~~ Decided: VID 0x16D0 / PID 0x0E9F stay, the official firmware's pair, kept by the maintainer's decision (2026-10-16, confirmed 2026-10-18), publication included; AUDIO mode is told apart by `bcdDevice`, with no second PID and no pid.codes PID (amendment 3).

## Amendments at acceptance (2026-10-11)
1. **No PC audio** (maintainer, 2026-10-11). The simulator renders UI only. Audio code is verified by host unit/oracle tests (V1) and QEMU (V3), then on hardware (V5). Decision 5 V4 and slice S3 are rewritten accordingly.
2. **D-OS-8 decided: retune the engine to exactly 44,100 Hz.**
   - **How:** I2S MCLK = 180 MHz × 196/3125 = 11,289,600 Hz = 256 × 44,100.
   - **Why:** the pitch shift against the 44,117.65 Hz clock is +0.69 cent, which is inaudible. In exchange, 44,100 Hz samples play at their true rate, the export no longer needs the dropped-sample kludge, and USB, MIDI clock and DAW timelines run exact.
   - **Gate:** `hw-session` item 2 must show SGTL5000 lock with no added noise or jitter against today's 16/255. If it fails, the engine keeps 44,117.65 Hz and Decision 2 (option A) applies unchanged.
   - **Code:** the rate lives in one constant (cycles per sample as a rational number), so either outcome changes one line.
3. **Open questions 1 and 3 decided.**
   - **Stem tap:** mono, pre-pan, after instrument, track FX and track level, through one `StemTap` shared by USB and SD stem export (this OS defines it; the official firmware's tap is an open research question).
   - **Default profile:** `10` behind the opt-in AUDIO mode, with `8` one tap away. Revisit after S5 item 3.
4. **Open question 4 decided** (maintainer, 2026-10-16, confirmed 2026-10-18): the VID/PID stay the official firmware's pair, publication included; no pid.codes PID and no second PID for AUDIO mode (amendment 3).
5. **Sample format:** the OS engine's internal format is decided in OS-005 and may be float. USB stays 16-bit at full speed because of bandwidth; Decision 1 does not depend on the engine being int16.

## Amendment 2 — what OS-006a built (2026-10-11, review fold)
- **Rate estimator:** an α-β tracking loop on CYCCNT per SOF, not a 32 ms average. A plain average shows errors of thousands of ppm under ±150 µs SOF jitter; the loop measured a worst case of 22.5 ppm (about 5 ppm 1σ) over 1.5 M simulated SOFs at ±500 ppm.
- **Stem pool:** 5 blocks per slot, not 2–3, which is about 13–14 KB for 10–11 channels. This SRAM figure replaces the 5–8 KB in Decision 3; the ADR 0013 D7 SRAM budget absorbs it.
- **Rings:** `usb::OverwriteRing`, an SPSC ring whose producer drops the oldest chunk, replaces `core::SpscRing`, whose producer cannot drop.
- **Nominal feedback:** 722,824 (0x0B0788) at 4080 cycles per sample; 722,535 (0x0B0667) after the 44,100 Hz retune (1,800,000/441). Both come from the one rate constant.
- **For the device driver (006b):** the AUDIO configuration is exactly 6 × 64 B, so EP0 must send a ZLP when wLength is larger. Whether FS feedback should be 3 or 4 bytes (TinyUSB pads to 4 for Windows) and the feedback `bInterval` stay hw-session items.

## Amendment 3 — USB identity (maintainer, 2026-10-11; the VID/PID kept by the decision of 2026-10-16, confirmed 2026-10-18)
- **Decision.**
  - **Name (maintainer):** the USB device name (`iProduct`) is **"Track3r"** in both modes. Interface and port strings use it as their prefix: "Track3r MIDI", "Track3r diag", "Track3r Audio".
  - **IDs:** both modes enumerate as VID `16d0`, PID `0e9f`, **the official firmware's pair, kept by the maintainer's decision (2026-10-16, confirmed 2026-10-18)**, publication included. The maintainer's statement is the evidence; the early test images used the same pair (`docs/hw/tracker-hardware-notes.md` §3). The VID belongs to another vendor, and a host tool that matches on VID/PID may take the unit for the official Tracker (risks accepted, `docs/specs/OS-USBDISK.md` §3.1). The OS is told apart by `bcdDevice` and the strings (manufacturer `track3r-os`, product `Track3r`).
- **AUDIO mode.** It uses the same VID/PID with a different `bcdDevice`: STANDARD `0x0001`, AUDIO `0x0101`.
  - **Why no second PID:** PIDs under `16d0` are allocated by the VID holder, so we cannot pick one ourselves.
  - **Why a different `bcdDevice`:** Windows caches descriptors per VID+PID+REV, so a different revision makes the host read the descriptor set again. The audio, MIDI and CDC functions bind to class drivers, so no vendor INF is involved.
  - A descriptor test asserts that the two modes differ in `bcdDevice`.
- **hw-session (S5 item 8):** switch STANDARD → AUDIO → STANDARD on Windows, macOS and Linux. Each switch must bind the right drivers, with no stale cache.
- **Note:** `0e9f` is not allocated to this project: it is kept on purpose, and no pid.codes PID is requested. Should the maintainer ever decide otherwise, only the `identity.h` constants change.

## Amendment 4 — what OS-006d built (2026-10-12)
- **S3 (engine) and S4 (audio on USB0)**, runtime only: `usb.mode` and `usb.profile` are not parameters yet and the `stem_map` chunk does not exist (persistent formats: maintainer decision; TODO OS-006e). STANDARD is the default at every boot; `diag usb mode|profile|map|audio` is the interface (no Config UI yet).
- **Stem tap:** every track strip defaults to route `MAIN | USB`, so the default mapping T1..T8 to S1..S8 needs no routing edit (staging happens only while the tap is on). The map (`audio/stem_map.h`) is the factory slots of P2/P8/P10/P11 plus per-slot edits; P4S is refused (the StemTap is mono, pre-pan).
- **Return:** the OUT stream reaches the engine through the **input strip** (summed, saturating, after the raw peak) rather than a routing-matrix row of its own; same level and routing bits as the line-in, level 0 until `diag mon`. The row is a later slice.
- **Buffers:** the IN stems packet and the feedback packet are armed one at a time, so both descriptors of each pair share one buffer (saves 1 KB of SRAM_U); the OUT pair keeps two. SRAM_U headroom over the 32 KiB floor of D7 is 640 B after this change.
- **Mode change:** detach (D+ off) after a 120 ms drain, 700 ms detached, descriptors rebuilt, `ControlPipe::rebind`, attach; run from the main loop, never blocking it. The feedback stays 3 bytes with `bInterval` 1 (hw-session items unchanged).

## Amendment 5 — USB RETURN: a capture-only descriptor set, and a duplex boot layout (OS-USBRET, 2026-10-13)
Overrides Decision 4's "a 2-channel OUT stream" as a fixed part of AUDIO, and amendment 2 point 3's default profile (below). Reversible; maintainer request: an option with 10 channels out, or 8 out and 2 in

- **Measured (0.4.6, Linux, the Tracker behind two hubs / a dock with a high-speed transaction translator).** With profile 10 (10 ch capture + 2 ch return + feedback) the host cannot open playback while capture is open (`aplay`: "Unable to install hw params"). Profiles 8 and 2 work duplex; profile 10 captures alone. (The 0.4.5 run, profile 10, had both streams running, IN 62,985 packets and OUT 4,313; its hub topology was not recorded.)
- **The setting.** Device setting **USB RETURN: On / Off** (`docs/formats/o3-settings.md`, field 13, appended by the row rule: no version bump; a file from before it reads On). It is a device property (what the host and its hubs can carry), so it is not in the project's `USBS` chunk, which is unchanged.
  - **Off:** the AUDIO descriptors carry **no OUT AudioStreaming interface, no return terminals and no feedback endpoint**. The function is capture only, so the host cannot open a return the bus has no room for. 6 interfaces (the MIDI function moves to 4/5), 6 endpoints on 4 numbers, a 293-byte configuration (384 B with the return). A descriptor test cuts the return out of the reviewed golden bytes by hand and compares.
  - **On:** the descriptors are the golden ones, byte for byte. STANDARD ignores the setting.
  - **Changing it** re-enumerates exactly like a profile change (`usb0::request_layout`; in STANDARD it is only remembered for the next AUDIO attach). The MIDI and CDC functions are unaffected except for the interface numbers of MIDI inside the AUDIO set.
- **What the user sees.** Layouts are named by the channels that leave the Tracker and the return that comes in: **"10 OUT"** = profile 10 with USB RETURN off; **"8 OUT / 2 IN"** = profile 8 with it on; "2 OUT", "2 OUT / 2 IN", "11 OUT", "10 OUT / 2 IN" (today's), "11 OUT / 2 IN". Console: `usb profile <2|8|10|11> [out|duplex]` (`out` = return off, `duplex` = on; without the word the setting stands), `usb return [on|off]`, and `usb mode`, `usb audio` print the layout name. Config gets a **USB** page (F5) with the USB RETURN row and its help; the mode and the profile have no Config row yet (the project's `USBS` and the `usb` command own them). Nothing is refused except profile 4S (still not fed by the engine): every other pair is valid at the bus level (table), and the ones that do not open behind a hub are a host limit, not an impossible descriptor.
- **Default.** The layout of every boot is now **"8 OUT / 2 IN"** (profile 8, return on), replacing amendment 2 point 3's default profile 10 (which said "revisit after S5 item 3"; the measurement above is that revisit): it is the pair that opens duplex on every host measured. Only the driver's boot layout changed (`usb0.cpp` `kBootProfile`, and the stem map is reset to it at start); the project model's own default state (`init_usb_state`, profile 10) and `audio::StemMap`'s class default are untouched and only matter for a project that carries `USBS` (it names its own profile). STANDARD is still the mode of every boot.
- **Bandwidth.** Full speed carries 1,500 B per 1 ms frame; USB 2.0 5.6.4 lets periodic transfers take 90 % (1,350 B). The isochronous payload of a frame is the sum of the `wMaxPacketSize` values (the 45-frame worst case); the bus bytes add 9 B per transaction (Decision 1's figure). A test derives every figure below from the built descriptors.

| Layout | Stems IN payload | + return OUT 180 + feedback 3 | Bus B/frame, return off | Bus B/frame, return on | % of 1,500 (off / on) | Behind the hubs [measured] |
|---|---|---|---|---|---|---|
| 2 | 180 | 363 | 189 | 390 | 13 % / 26 % | duplex opens |
| 8 | 720 | 903 | 729 | 930 | 49 % / 62 % | duplex opens |
| 10 | 900 | 1,083 | 909 | 1,110 | 61 % / 74 % | capture opens; duplex does **not** |
| 11 | 990 | 1,173 | 999 | 1,200 | 67 % / 80 % | not measured; capture expected [inf], duplex expected not to open |
| 4S | 900 | 1,083 | 909 | 1,110 | 61 % / 74 % | not fed by the engine |

  Every row is under the 1,350 B bus limit, so the host's refusal is not the bus total. [inf, to confirm] A hub's transaction translator places each full-speed isochronous transaction in microframes; Linux's EHCI scheduler budgets only part of a frame for it (from memory: full microframes 0 to 5, a short slice of 6, none of 7: about 78 %) and a 900-byte packet needs a complete-split in about five consecutive microframes, so the IN stream plus the OUT packet plus the feedback packet cannot all be placed, while 720 + 183 can. A direct connection has no translator in the path, which would explain the 0.4.5 run (topology not recorded) having both streams.
- **wMaxPacketSize at the 44.1 kHz asynchronous worst case (45 frames a packet).** Checked for every profile, return setting and speed against the built bytes: the stems endpoint is `channels x 2 x 45` (180, 720, 900, 990, 900), the return endpoint 180, the feedback endpoint 3 (FS) / 4 (HS), none above 1,023 (FS) or 1,024 (HS), the validator rejects a smaller or larger value, and the driver buffers hold the largest packet (IN 990 B in an 11 x 45 x 2 buffer, OUT 192 B in a 48-frame buffer against 180). **No oversize found; nothing changed.**
- **Hardware checks (not done).** Linux behind the same hubs: "10 OUT" capture alone (`arecord -D hw:... -c 10`), and "8 OUT / 2 IN" with `aplay` and `arecord` open at once; the same on a direct port. macOS and Windows: enumerate "10 OUT" and "8 OUT / 2 IN" (no stale driver after the switch, the MIDI function still bound at its new interface numbers 4/5 in "10 OUT"), and Windows shared mode with each. `usb return off` then `on` from the console and from the Config page; the setting written to the file is read back at boot.

## Amendment 6 — the USB return has its own route and level, and no loopback (OS-006e, 2026-10-13)
Overrides Decision 4's "a planar stereo source ... in the routing matrix, so it reaches MAIN, CUE and REC like Line L/R" and amendment 4's "summed into the input strip". Reversible; new Community OS behaviour (no parity claim). Evidence: a hardware session, 0.4.5 and 0.4.6: with the monitor at its default (0) a DAW playing into the Tracker was silent, and with `mon on` a -6 dBFS tone read -23.6 dBFS on the master where -18 was expected.

- **Route.** The host's stream no longer joins the input strip. It is heard on the **outputs** (line out and headphones) at its own level times the master level, whatever the strip's level, routing bits and sends (the strip is the line-in, microphone or radio alone). Default: **unity**, so that by default a DAW that plays into the Tracker is heard at the level it sends. The recorder's USB source (`InputFeed::ReturnOnly`) still records the stream through the strip; while it does, the direct route ramps shut over one block (and open again after it: no step) so that the monitor does not double it.
- **No loopback by default (maintainer decision, from the review).** The return is added to the heard planes at the very end of the render (`Graph::run_levels`), after MAIN's chain and master level, and is in **no capture**: not in MAIN (so not in the MAIN meters, the stems' **Master L / R**, or `RecTap::Source::Master`, which resamples MAIN), not in CUE and not in REC. Otherwise a DAW that monitors its Tracker input while it plays into the Tracker would build a sustaining feedback loop by default, and a backing track would be recorded back. It bypasses the master limiter (the heard sum saturates once, at full scale); the master meters, the scope and the spectrum see it, as they see what is heard. In OUT MODE MAIN-L / CUE-R it is part of the left jack's mono sum. CUE and REC are not routes of the return (a routing-matrix row, when the Routing screen has the matrix). **Follow-up, not built:** a loopback setting (device row 17) for a user who wants the DAW in the stems and the resample.
- **Level.** The device setting **RETURN LEVEL** (0..100 on the mixer level law, 100 = unity = default; `docs/formats/o3-settings.md`, field 16, appended by the row rule behind two reserved rows: no version bump; a file from before it reads 100), on the Config USB page below USB RETURN. A device-level choice (the interface's calibration, like USB RETURN), learnable. The main loop tells the engine (`ParamChange` master `UsbReturnLevel`, ramp 0: one block of smoothing) at the first pass and at every change of the setting.
- **Why not the Mixer.** The Mixer's rows are the project's (the IN strip's level is in the MIXR chunk); a project saved at home would then change what the host is heard at in the studio, and a new strip is a new MIXR field with a compat rule and a new UI row for a control that is set once. The USB page already exists and holds the setting that decides whether the return exists at all.
- **The 5.6 dB (the level bug).** The master DRY level scales every strip's fader, the input strip included, and it is **86 in every v7+ project** (`kDefaultVolume`): 10^((86 - 100) / 50) = 0.525 = -5.6 dB. The `diag mon` level of the input strip (70, -12 dB) times it is -17.6 dB, which is what was measured (-6 - 12 - 5.6 = -23.6). Nothing else in the path takes level (the limiter is transparent under its ceiling: tests). The input strip is now outside the DRY **level** (it is not a track of the song; DRY MUTE still mutes it, SOLO already did not touch it), and the return is outside both.
 - **The exemption in this OS's routing.** The dry-mix volume scales the **track amplifiers only**; the input strip (line-in, microphone, radio: routed through the codec's input path) is outside it. Whether the official firmware's dry-mix volume leaves its line input alone is an open research question (Q). PAR-041 (release 1.3.0, documented in the release notes, not verified) gives the line input its own MIDI CC (82) apart from the dry mix (81).
  - **Release note.** A v7+ project with a saved input level above 0 is now **5.6 dB louder on the input strip** than the 0.4.6 build played it (its master DRY level 86 no longer scales it), and `mon on` goes from -17.6 to -12 dB. That is 5.6 dB less feedback margin for a microphone monitored through MAIN (a mic next to a speaker): raise the monitor with care. The USB return is not in MAIN, so it adds no such loop.
- **Cost.** Per block with a host streaming: 2 `kernel::mac` + 2 `kernel::sat_clear` of 64 samples for the heard sum (plus 2 `kernel::mac` of MAIN when it has signal) in place of the 128 saturating adds into the strip's planes; one atomic load with no host. `Graph` is 288 bytes larger (the return block, a smoother, a level and flags); the hot code (SRAM_L) is within 12 bytes of its 26 KiB budget after this (mem_map.h kSramLHotBudget), so the next hot addition must make room.

## Amendment 7 — the unit comes up as the audio device, by a device setting; changes apply on a confirmed press (OS-USBAUDIO, 2026-10-14)
Overrides amendment 4's "the mode is runtime only, STANDARD at every boot" for the boot (the maintainer: on by default, but changeable from Config; new Community OS behaviour, no parity claim; reversible; Level C because it moves the USB start in the boot path and appends a persistent setting). Evidence: a hardware session had to type `usb mode audio` on every boot, and changed USB RETURN with the encoder by accident, which re-enumerated the unit at once.

- **The setting.** Device setting **USB AUDIO: Off / On, default On** (`docs/formats/o3-settings.md`, field 17, appended by the row rule: no chunk, no version bump; a file from before it reads On). Off is STANDARD (MIDI and the serial console), On is AUDIO. A device-level choice, so not in the project.
- **Boot: one enumeration.** The USB port is started after the card is read (`main.cpp`: audio, SD mount and `Settings/device.o3st`, then `appimg::start_usb`), so the first descriptor set the host sees is the setting's: `app::usb_boot_layout(setting, profile, usb_return)` gives AUDIO when the setting is On and the engine can feed the profile, with the boot profile 8 and the saved USB RETURN, else STANDARD; `usb0::start(mode, profile, usb_return)` rebuilds the descriptors before the pull-up. A card that is absent or unreadable gives the defaults (On); no app at all (the display or the asset upload failed) and the tone images boot STANDARD, the console only. The cost of the order: the host sees the unit when the card has been read and not before: tens of ms with a healthy card; for a card that does not answer, the worst case is the init (`worst_init_us()`, under 3 s) plus up to 0.5 s (`worst_read_call_us()`) for each read call that fails (the mount's reads, then the settings file's). A crash inside the mount no longer leaves a console up for that boot, and a crash loop there (a card that makes every boot crash in the mount) gets no console at all: the recovery is to remove the card (the unit then boots without it, enumerates with the defaults and has its console); the crash record is read on the next boot's console, as before. The "saved layout" at boot is the profile the driver starts with, 8: nothing but the project's `USBS` part saves a profile, and the project is read after USB is up, so a project whose profile is not 8 re-enumerates the unit once after its load (in AUDIO; in STANDARD a profile is only remembered).
- **The setting decides the mode, the project the profile and map.** A project's `USBS` mode byte used to override the boot mode; with the boot now decided by the setting a project saved in the other mode would either defeat Off across a reboot or enumerate twice with On. So `app::usb_apply_target` takes the mode the unit presents (or is changing to: the setting's at boot, or a runtime `usb mode` since, which a load must not undo) and the profile and stem map from the project (`o3-project.md`; the chunk and its bytes are unchanged). The mode byte is also left out of the comparison that stores the runtime state in the project, so a USB AUDIO apply or a `usb mode` neither marks the project unsaved nor changes its `USBS`. Reversible: two small functions.
- **Runtime.** `app::sync_usb_audio` carries a change of the setting to the driver, like USB RETURN's, asking for the mode together with the saved USB RETURN and running before the return's sync, so one Config APPLY of both settings is one re-enumeration. **The console's `usb mode audio|standard` is runtime only and does not write the setting** (a reboot comes up as the setting says): the maintainer's setting is Config's, and a console command used to try a layout must not silently change what the unit does at the next power-on.
- **Press to apply (USB AUDIO and USB RETURN).** The rows carry `kParamApply`. The encoder (and Delete) changes a pending value that the page shows starred, in the accent colour; **F7 (APPLY)** says "USB WILL RECONNECT: PRESS AGAIN" when the apply will re-enumerate (a USB AUDIO change always; a USB RETURN change only while AUDIO is presented, STANDARD's descriptors do not depend on it; otherwise "PRESS AGAIN TO APPLY"), the second press writes every pending value (one re-enumeration for both; "USB RECONNECTING" only if a write that re-enumerates took, else "APPLIED"); a move of the cursor takes the first press back, and leaving the page, the Config button's page cycle or another screen discards the pending values. RETURN LEVEL stays live. (The encoder has no push switch, `docs/design/product-ux.md` Q7, so the confirming press is a front-panel F-key.)

## Amendment 5 — D-OS-8 is the default clock (2026-10-23, OS-RATE)
- **Decision:** the engine runs at exactly 44,100 Hz by default in release 1 (maintainer, 2026-10-23). `O3OS_RATE_44100` defaults to ON: `1800000/441` cycles per sample, MCLK = 180 MHz x 196/3125 = 11.2896 MHz. The pre-1.0 clock (4080/1, 44,117.647 Hz) is still built with `-DO3OS_RATE_44100=OFF`. The text above that names 4,080 cycles, 44,117.65 Hz or the feedback value 722,824 describes that clock; the default nominal feedback is 722,535 (0x0B0667).
- **Gate passed [unit, verified]:** image fqa.4r (dev 5ff06aed, option ON): the true clock measured 44,100.00 Hz against 44,117.58 Hz on fqa.3 (the old clock); USB audio ran 60 s with no xrun or click; the CPU is unchanged; the MIDI clock is its own timer (48.0006 ticks/s on both). The `hw-session` gate of amendment 2 (lock and no added noise) therefore holds, and the fallback to 44,117.65 Hz is not taken.

## Amendment 8 — the number of USB outputs is a per-project choice: 2, 4, 6, 8, 10 (OS-USBOUTS, 2026-10-29)
New Community OS behaviour; maintainer request, decisions D1-D4 and defaults O1-O3 in `docs/specs/OS-USBOUTS.md`. Adds to amendment 5; nothing in it is undone.

- **What changes.** The profile a project stores in `USBS` can now also be **P4** (T1, T2, Master L/R) or **P6** (T1..T4, Master L/R), chosen on the unit in Mixer > STEMS (row OUTPUTS). A change re-enumerates the port like any profile change (`usb0::request_layout`) and resets the stem map to the new profile's factory slots. The default of every boot stays "8 OUT / 2 IN" and a project without `USBS` changes nothing. Profile 11 stays console-only.
- **The return at 10 channels is never presented.** USB RETURN stays the device setting (amendment 5). The descriptors carry the return only when the setting is On **and** the profile has fewer than 10 channels (amendment 5: "10 OUT / 2 IN" does not open behind a hub with a transaction translator). The driver keeps the *wish* (the setting) and the *presented* value (`wish && channels < 10`) apart: setting On at 10 channels stores nothing different, and going back to 8 channels brings the return back.
- **File.** A project with 4 or 6 outputs writes `USBS` 1.1 (P8 with 8 slots, then one byte with the real count, which a 1.0 reader skips and so reads P8); 2, 8, 10 and 11 stay 1.0 byte for byte (`docs/formats/o3-project.md`).
- **Bandwidth.** Same derivation as amendment 5 (the test builds every figure from the descriptors).

| Layout | Stems IN payload | + return OUT 180 + feedback 3 | Bus B/frame, return off | Bus B/frame, return on | % of 1,500 (off / on) | Behind the hubs |
|---|---|---|---|---|---|---|
| 4 | 360 | 543 | 369 | 570 | 25 % / 38 % | not measured; duplex expected to open [inf] (below the 8-channel layout that does) |
| 6 | 540 | 723 | 549 | 750 | 37 % / 50 % | not measured; duplex expected to open [inf] |

- **wMaxPacketSize:** `channels x 2 x 45` (360 and 540 at full speed, 48 and 72 at high speed). The goldens for P4 and P6 (FS, HS; with the return, and the return-off sets cut out by hand) are derived by hand from the reviewed P8 arrays.
- **Hardware checks (not done).** Linux enumerates each of 2, 4, 6, 8 and 10 channels with the return on and off (`arecord -l`, a capture and a duplex run); 10 channels with the setting On presents no return.
