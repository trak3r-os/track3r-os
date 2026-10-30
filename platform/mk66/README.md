# platform/mk66: the MK66 platform layer and the skeleton image (OS-007b)

The Tracker OG's MCU (MK66FX1M0: Cortex-M4F, 1 MiB flash, 256 KiB SRAM, 16 MiB SDRAM) brought up from reset with nothing
but this tree's code: a vector table with every handler named, the reset sequence, the 180 MHz clock tree, the SDRAM, the
interrupt priority table, SysTick and the cycle counter, fault handlers that leave a crash record, and a `main` that idles in
WFI with a heartbeat. OS-005a adds the audio path (I2S0, eDMA, the SGTL5000, the render interrupt: "Audio" below); since OS-007c
it also brings up the FT812 display and draws the OS app's pattern view on it (section "The display", below); since OS-006b the USB0
port enumerates (MIDI and a CDC serial port) and answers the `diag` console (section "USB and the `diag` console", below); since OS-007e the SD card
is mounted and read (read only for everything except the firmware writer of OS-006c) and the OG project in its `Workspace/` folder is loaded at boot (section "SD card", below); since OS-006c
the `diag` console can put a `.ptf` on the card and hand over to the bootloader (any writable FAT card since OS-FWANY) (section "Firmware update over USB", below).

**Nothing here is flashed, copied to removable media or handed over.** `o3os.hex` is an artifact of a build, not a
candidate: a candidate needs the `fw-gate-check` skill and the maintainer's decision (the flashing-safety rules). The code is written fresh from the K66 Sub-Family Reference
Manual's register facts and the parts' data sheets, as a from-scratch implementation; the pin facts below are facts of the Tracker board.
NXP's `MK66F18.h` was used only as a fact table to cross-check addresses and field positions.

## Build and check

```sh
cmake -S . -B build-os-mk66 -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/mk66-toolchain.cmake -DO3OS_PLATFORM=mk66
cmake --build build-os-mk66 && ctest --test-dir build-os-mk66
```

Builds `o3os.elf` (`build-os-mk66/platform/mk66/`), then `o3os.hex` and `o3os.bin` behind the static image checks (below), and
prints the linker's per-region usage and `size -A`. Options (both for hardware-session images, never silently on):

| Option | Default | Meaning |
|---|---|---|
| `O3OS_MK66_SDRAM_FULL_TEST` | ON | write and read back every SDRAM cell at boot (two write-and-verify passes, a few seconds by estimate, LED on; MK66-1 measures it); the wiring tests always run |
| `O3OS_MK66_FAULT_DRILL` | OFF | raise one deliberate UsageFault 10 s after power-up to prove the crash-record round trip on hardware (a separate image) |
| `O3OS_AUDIO_TEST` | OFF | the HW-A variant (OS-005a): after about 0.6 s of clean audio the outputs are unmuted and the test tone fades in (left channel 1 kHz alone for about 3 s, then the right joins at 1.5 kHz, -20 dBFS). the app has no sound of its own. OFF: the outputs are muted at reset and open once at boot, after the SD mount, the project load and 0.6 s of clean audio (OS-OUTON; OS-005d opened them on the first Play), so Play starts the demo pattern at once |
| `O3OS_BENCH` | OFF | OS-BENCHOPT: the CPU bench (`diag bench <kernel\|all>`, `diag cache check`; `src/bench.cpp`) is in the image. OFF: a release image carries none of it (`diag bench` and `diag cache check` answer `ERR ... is not in this image`). A bench image for the tester is built with `-DO3OS_BENCH=ON`; the HIL scenarios that read `diag bench` (qa_global, qa_pattern, qa_config, the OS-CPUOPT1 rows) need one. `tools/ci.sh mk66` builds it (`mk66-bench`) so that it does not rot |
| `O3OS_AUDIO_TEST_STALL` | OFF | with `O3OS_AUDIO_TEST`: one render overruns on purpose 20 s after the tone starts (HW-A F-1: left and right after an underrun) |
| `O3OS_SGTL5000_I2S_CTRL` | `0x0130` | the codec's I2S control word: the value proven on this board, or `0x0030` (SCLKFREQ 64 Fs, HW-A E-1); nothing else is accepted |
| `O3OS_RATE_44100` | ON | an option of the whole tree: the audio clock at exactly 44,100 Hz (D-OS-8, the default since v1.0.0, verified on the unit 2026-10-23), `1800000/441` cycles per sample and MDR 196/3125. OFF builds the pre-1.0 clock, `4080/1` and 16/255, 44,117.647 Hz (HW-A image C); it changes one constant (`core/audio_clock.h`), every target gets it |
| `O3OS_DEMO_CYCLE` | OFF | OS-007c: walk the pattern cursor down every 1.5 s and cycle the three themes, so the hardware session sees the screen move (it works beside the real input) |
| `O3OS_INPUT_TEST` | OFF | OS-007d: start on the hidden INPUT TEST screen with no boot chord (an image for the hardware session; the default image opens it from Shift + F1 + F8 held at power-up), see "Input" |
| `O3OS_DISPLAY_ROTATE` | 1 | OS-007c: panel orientation, `REG_ROTATE` 0..7 (0 landscape; 1 is the inverted landscape that shows upright on the maintainer's unit): see "The display, Orientation" |

The pure parts (memory map, crash record, memory tests, clock and SDRAM register values, LED codes, interrupt map, and since OS-007c
the display driver's register facts, panel, SPI and DMA values, chip bring-up and `hal::Display`) also build
in the host and QEMU trees as `platform_mk66_pure`; their tests are in `tests/` and run in `unit` (host, ASan/UBSan) and
`qemu_mk66` (Cortex-M4F code generation). Since OS-007d the input drivers' pure parts (the key matrices, the TCA8418 protocol, the debounce, the
encoder's detents, the scan, the IS31FL3731 pad LED driver) and the board's input facts (`mk66_input_*_test.cpp`) are in the same library and suites.
`ctest` in the MK66 tree runs: `mk66_gen_ld_selftest`, `mk66_image_selftest`, `mk66_o3console_selftest` (the host tool for `diag`, OS-006b),
`mk66_ld` (the generated script against the header), `mk66_image` (the HEX checks), `mk66_flashconfig`,
`mk66_noinit_guard` (the noinit macros accept what they should and the compiler refuses an initialised object), and for the
audio path `mk66_ramfunc_selftest` and `mk66_ramfunc` (the hot path in SRAM_L calls nothing in flash, see "Audio"), and
`mk66_dma_owner_selftest` / `mk66_dma_owner` (a source check, also in the host build: only `boot::dma_init()` writes `DMA_CR` and the
`DCHPRI`s, it is called once from the reset sequence, and the display's eDMA engine writes neither). The audio
path's pure parts (`audio_config.h`, `sgtl5000.h`, `resources.h`, `diag_report.h`) are tested in `tests/mk66_audio_config_test.cpp`,
`mk66_sgtl5000_test.cpp`, `mk66_resources_test.cpp`, `mk66_diag_report_test.cpp`.

## Memory map (`include/.../mem_map.h` is the single source)

| Region | Base | Size | Use |
|---|---|---|---|
| FLASH | `0x00000000` | 1 MiB | vectors at 0, flash configuration field at `0x400`, code, constants, then the load images of `.ramfunc_l` and `.data` |
| SRAM_L (linker region) | `0x1FFF0000` | 48 KiB | `.ramfunc_l`: hot code, copied from flash at reset (the heartbeat LED function, and since OS-005a the audio ISRs and the pipeline: budgeted at 24 KiB, 20 hot kernels + 4 audio HAL/ISR); behind it `.tapmem_l` (NOLOAD, OS-005b review): the audio taps' data, one `audio::TapMem` (the stem pool and ring, the scope, spectrum and meter rings), budgeted at 24 KiB, constructed before `main` (the reset sequence does not clear it) |
| module slab (reserved) | `0x1FFFC000` | 16 KiB | top of SRAM_L, ADR 0002 / OS-005i; not in the linker region, so nothing else can land there |
| SRAM_U | `0x20000000` | 192 KiB | `.noinit` (crash block 128 B first), fault stack 512 B, `.dma_rings` (the audio DMA rings, 1 KiB aligned, NOLOAD), `.data`, `.bss`, free (at least 32 KiB, a linker assert), main stack 24 KiB at the top (initial SP `0x20030000`) |
| SDRAM | `0x70000000` | 16 MiB | the ADR 0013 D7 table, eleven `constexpr` regions summing to exactly 16 MiB; only its last 192 KiB (the slack, `0x70FD0000`) is a linker region (`.noinit_sdram`, attribute `O3OS_SDRAM_NOINIT`) |

**SDRAM regions (OS-005a; the project and sample-arena rows differ from ADR 0013 D7, see below).**

| Region | Base | KiB | Use |
|---|---|---:|---|
| `sample_arena` | `0x70000000` | 10,816 | samples, recordings, edit copies (one arena; a recording is limited to the free space: 125 s when empty) |
| `delay` | `0x70A90000` | 768 | delay lines |
| `reverb` | `0x70B50000` | 256 | reverb lines |
| `module_region` | `0x70B90000` | 768 | cold code/data of loaded modules |
| `project` | `0x70C50000` | 1,920 | the project model arena (the model worst case is 1,933,504 B = 1,888.2 KiB, OS-APPUX S3, E-CFG-03 and E-SONG-07) |
| `undo` | `0x70E30000` | 384 | undo journal |
| `file_job` | `0x70E90000` | 512 | file and job buffers |
| `lut` | `0x70F10000` | 192 | boot-generated tables |
| `ui_cache` | `0x70F40000` | 512 | UI cache |
| `diag_log` | `0x70FC0000` | 64 | session log |
| `slack` | `0x70FD0000` | 192 | 1.2 %, the linker's `.noinit_sdram` window |
| | | **16,384** | exactly 16 MiB (`static_assert`, `mk66_memmap`, the `mk66_ld` check) |

The orchestrator's correction after OS-004a: ADR 0013 D7 budgeted 1,280 KiB for the project and the model's worst case does not fit, so
the project region is 1,920 KiB and the sample arena gave up the 640 KiB (11,456 to 10,816 KiB); the slack is unchanged.
`mem_map.h` `static_assert`s the region is at least `kModelArenaWorstCaseBytes` (a mirror of `o3os::model::kModelArenaBytes`, which the
platform layer may not include; `tests/mem_map_model_test.cpp` checks the mirror).

OS-FU2: the Sample load audition's preview buffer (`kPreviewSamples` = 4 s at 44.1 kHz, 345 KiB, `app_main.cpp`) is carved out of the sample arena's tail, so a project that nearly filled the arena has that much less room for samples (`load: sample arena full` arrives 345 KiB earlier).

**SRAM budgets (ADR 0013 D7), row by row in `mem_map.h` (`kSramLBudget`, `kSramUBudget`), printed by `diag mem`.** SRAM_L: hot kernels
20 KiB + audio HAL/ISR 4 KiB (the linker asserts `.ramfunc_l` stays within these 24 KiB) + 24 KiB of tap data (`.tapmem_l`, asserted likewise: the render writes it a block at a time for the USB interrupt and the UI, so it sits away from the SRAM_U the DMA masters use) + the 16 KiB module slab. SRAM_U: audio rings
2 KiB, block buffers 14, voice state 16, module hot state 16, sequencer 8, rings 16, SD/FT812 staging 24, UI/app `.bss` 18 (39 until OS-MEM1), stack 24,
crash block + fault stack 0.6 KiB: 138.6 KiB (159.6 until OS-MEM1), so at least 32 KiB stay free (a `static_assert` and a linker assert). Deviations from the
ADR table, all small: the vector table stays in flash (no SRAM_L vectors row); the crash block and fault stack get a row paid for by 1
KiB off the UI/app estimate; the audio rings are 2 KiB, not 1.5, because the eDMA wraps addresses with a modulo (power-of-two rings) and
the row covers the 3-slot fallback too.

### SRAM map (OS-MEM1: what lives in internal SRAM and why)

Measured from the image's ELF (`arm-none-eabi-nm -S --size-sort -C build-os-mk66/platform/mk66/o3os.elf`, filtered by address; the build's region report
prints the totals). Classes: **render/ISR** = touched by the render or an ISR per sample/block/interrupt (stays); **DMA** = a bus master (eDMA, USB) reads or writes it,
so it must be in SRAM and aligned as declared (stays); **main** = main loop only (movable to SDRAM, `SdramBox` / `O3OS_SDRAM_NOINIT`).

Before and after OS-MEM1 (incremental builds of the same tree, `size` of the image):

| | before | after |
|---|---:|---:|
| SRAM_U image (`.noinit` + fault stack + rings + `.data` + `.bss`) | 139,056 B | 100,472 B |
| SRAM_U headroom over the 32 KiB floor (192 KiB - image - 24 KiB stack - 32 KiB) | 208 B | 38,792 B |
| SRAM_L hot code (`.ramfunc_l`, of 26 KiB) | 26,364 B (260 B room) | 23,468 B (3,156 B room) |
| SRAM_L tap data (`.tapmem_l`) | 22,272 B | 22,272 B |
| SDRAM slack window (`.noinit_sdram`, of 192 KiB) | 0 B | 38,592 B |
| flash image | 407,324 B | 407,284 B |

**SRAM_U, by object (the image after the moves; every object of 500 B or more, the rest grouped).**

| Object | Bytes | Class | Who touches it, and why it is where it is |
|---|---:|---|---|
| `g_midi_storage` (seq::MidiLink) | 21,736 | render/ISR | the emitter's rings (PIT ISR), the UART and USB rings, the router, the clock-follow loop: written by the render, read by three ISRs |
| `g_engine` (audio::Engine + Graph) | 13,592 | render/ISR | the render itself: strips, ramps, accumulators, block planes |
| `g_display` (Ft812Display) | 12,464 | DMA | two `alignas(8)` staging buffers (frame 8 KiB + upload 4 KiB) that the SPI eDMA reads; the eDMA's reach into the FlexBus SDRAM is not established in this repository (no K66 RM text here), so they stay in SRAM_U |
| `g_voices` (VoicePool) | 8,768 | render/ISR | the voice table and block buffers, read per sample |
| `g_seq_storage` (seq::Sequencer) | 8,728 | render/ISR | ticked from the render line (E-SONG-07: its copy of the song list, 656 B, is `g_seq_order` in SDRAM, SRAM_U keeps its 32 KiB free floor). The ADR 0013 D7 row is 8 KiB and `app_main.cpp` allows 640 B above it (512 B of slack, +128 B for OS-INSTUI S8b: the MIDI instrument's output state, 72 B: the program of each channel, the routes a bend went to, the route of the note each track sounds); 8,632 B before S8b, 8,704 B after (the pads' route words are in `.bss` of the MIDI path, +96 B) |
| `app::g_scratch` (params screen curve scratch) | 4,480 | main | portable app code (`params_screen.cpp`, also in the simulator): left, a follow-up could give it a platform placement hook |
| `usb0::g_cdc_tx` / `g_cdc_rx` | 4,364 / 1,100 | render/ISR | the CDC rings between the console (main loop) and the USB interrupt (priority 64) that drains and fills them |
| `update::device::service()::svc` / `store` | 3,408 / 1,640 | main | the firmware-update service; in `fw_host_mk66.cpp` (not touched by OS-MEM1: the update path is the storage engineer's) |
| `g_events` (SeqEvent block) | 2,304 | render/ISR | what the sequencer hands the engine a block at a time |
| `usb0::g_dev` | 2,288 | render/ISR | the USB device driver object, run by the USB interrupt (`g_dev.isr()`) |
| `storage::g_fat` + `DirBuf` + `LfnBuf` + `g_card` | 2,264 + 608 + 512 + 160 | main | the FAT and card layers: polled SDHC (no ADMA, resources.h), so CPU-copied and movable later; left, they are the storage engineer's files |
| `usb0::g_return_ring` | 2,060 | render/ISR | the USB stream return the render pops |
| `input::scanner()::s` | 1,816 | main | the key scanners, polled from the main loop (movable, small) |
| `g_rx` / `g_tx` / `g_block` | 1,024 / 1,024 / 128 | DMA | the audio DMA rings (1 KiB aligned `.dma_rings`) and the block descriptor |
| `o3os_audio_pipeline` | 784 | render/ISR | the render's pipeline object |
| `usb0::g_bdt` (512 B, 512-aligned), `g_desc`, `g_midi_*_host`, `g_pipe`, ... | about 2,900 | DMA | the OTG module's buffer descriptor table and packet buffers (usb/soc_facts.h: SYSMPU master 4, plain SRAM_U) |
| everything smaller (about 90 objects) | about 4,000 | mixed | counters, flags, the console's shell and pager |

Moved by OS-MEM1 (all main loop only, now in the SDRAM slack window through `SdramBox<T>` or a `memset` before the placement new: `sdram_box.h`): `g_app_storage`
(app::App, 28,008 B), `g_loader` (OgFolderLoader, 8,336 B), `g_sample_loader` (OgSampleLoader, 2,240 B): 38,584 B. The SDHC driver is polled and the display copies
the App's display-list words into its own staging buffer, so no DMA master reads any of them. SDRAM stays 16 MiB exactly; the slack window is a linker region with an overflow assert.

**SRAM_L `.ramfunc_l` (23,468 B), by group** (symbols: `nm -S -C` of the same ELF between `__ramfunc_l_start` and `__ramfunc_l_end`):

| Group | Bytes | Rate |
|---|---:|---|
| VoicePool (render, filters, envelopes, note start/stop) | 5,726 | per voice per block; `note_on`, `filter_apply`, `fmod_start` (1.8 KB together) run per note event and are the next candidates |
| kernels (gain, mac, pan, saturate, meters, converters, test tone) | 4,976 | per block, per sample |
| Graph (strips, buses, chain, levels, taps, drain) | 4,916 | per block |
| Engine, Pipeline, AudioStatus | 3,258 | per block; `Engine::play_live` (1,076 B) is entered every block to look at the live queue |
| MIDI emitters (DinEmitter, UsbMidiOut) | 2,014 | PIT and render context, jitter-sensitive |
| ModuleVoices | 1,068 | per block |
| taps and meters | 762 | per block |
| ISR entry points (DMA13/29, UART4, PIT0, SWI, led) | 736 | per interrupt |

**The exception list of "SRAM_L calls nothing in flash".** A function marked `O3OS_AUDIO_COLD` (`audio/hot.h`) is in flash and called from render-context code in
SRAM_L through a register (`long_call`, `blx rN`), which `check_ramfunc.py` allows like any indirect call; the list is exactly that macro's uses, each documented with its call
rate beside the declaration: `Graph::apply_strip`, `apply_master`, `recompute_strip`, `recompute_master` (OS-MEM1: 2,912 B; per ParamChange popped by `drain()`, at most 64 a
block and normally none, and per strip marked dirty, at most 13). Cost when they run: flash instead of SRAM_L fetch for a few hundred instructions in a block that carries
a parameter change; HW check `diag cpu` before and after, an idle block's render time must not move.

`mk66.ld` is generated at build time by `tools/gen_ld.py` from `mk66.ld.in` and `mem_map.h` (it evaluates the header's
`inline constexpr std::uint32_t` constants with a whitelist, refuses anything it cannot, and verifies the result: regions,
overlaps, the SDRAM sum). The script asserts the layout it relies on: vector table below `0x400`, `.flashconfig` exactly
16 bytes at exactly `0x400`, the crash block first in `.noinit`, `.bss` clear of the stack, initial SP `0x20030000`, hot code
below the module slab. `--orphan-handling=error`: a section the script does not place fails the link. There is no heap, no
`_sbrk` and no `exit`: a library function that wants one is a link error.

**The flash image has no hole.** Whether the SD bootloader tolerates a hole is unknown, so
this one is a single run from 0 to its end: the load image of `.data` starts at an explicit address right behind the one of
`.ramfunc_l` (a plain `AT > FLASH` would align it to 8 and leave a hole), the script asserts that the loaded sections' sizes
add up to the span of the image, `objcopy` runs with `--gap-fill 0xFF` as a backstop, and `check_image.py` refuses any hole
(`contiguous`). Because that address is explicit, `ld`'s `FLASH:` line in the memory-usage table leaves out the `.data` load
image; the flash use is the `image: ... N bytes` line of the check output (and `__image_end`).

**Objects that must not be initialised** (the crash block, `O3OS_NOINIT`, `O3OS_SDRAM_NOINIT` in `sections.h`) go in input
sections named `.bss.noinit...`: GCC makes them NOBITS and refuses a non-zero initialiser in them at compile time
("only zero initializers are allowed in section"), so a value can never be silently dropped from a NOLOAD output section. The
plain names `.noinit` and `.noinit_sdram` are not matched by the script, so an object put there by hand is a link error.
Still silent, and the author's to know: a zero initialiser (`= 0`) is accepted and not applied; a constructor does run.

**No stack guard.** The main stack (24 KiB at the top of SRAM_U) grows down towards `.bss`, and nothing detects an overflow:
it corrupts `.bss` and `.data` silently. (The QEMU layer has an MPU guard, but the K66's Cortex-M4 has none: NXP's device header
says `__MPU_PRESENT 0`.) Later item, not in this slice: a guard region at the bottom of the stack area through the K66's
**SYSMPU** (the bus-master MPU, `0x4000D000`, no access for the core), whose precise BusFault lands in the fault stub, which
already runs on its own stack; validate it on hardware (ADR 0002 validation 5); and a painted stack with a high-water mark for
`diag mem` (ADR 0013, hazard H14). The 512-byte fault stack is unguarded too.

Deviation from ADR 0013 D7: the vector table stays in flash (named handlers, nothing patched at run time), so the "vectors 1
KiB" of SRAM_L is not used; SRAM_L is 48 KiB of hot code and tap data (24 + 24 KiB budgets) plus the 16 KiB slab.

## Reset sequence (`src/startup.cpp`, `src/boot.cpp`)

1. `cpsid i`; **watchdog unlocked (`0xC520`, `0xD928`) and switched off** (`STCTRLH = ALLOWUPDATE`): the Reference Manual's unlock sequence.
   `ALLOWUPDATE` keeps the control
   registers writable for the later, deliberate re-enable.
2. FPU on (`CPACR`).
3. **Pins quiet** (the table below): port clocks on, then **the power key PTA24 HIGH** (PCR `0x144`: high drive, slew-rate limited; OS-BUG-PADS step 3, **untested on
   both units**) as the first pin action, so that no quiet pin is driven into a rail that is still off, then **audio mute key (PTD6) LOW = muted** at once
   (the amplifier stays off until the audio path runs), then a 2 ms wait (`board::kRailSettleUs`) for the rail, then
   the status LED off, the analogue input switch in the receiver's position (LOW; see the pin table), the radio in reset, the FT812's chip
   select and power-down released, the MIDI output idle. The crash context is reset to stage Reset, no module.
4. C runtime: `.data` and `.ramfunc_l` copied from flash, `.bss` cleared; `.noinit` untouched. Stage, reset cause
   (`RCM_SRS0/1`) and any crash record are captured.
5. DWT cycle counter on, then **clocks**: 16 MHz crystal (OSC0 10 pF, range 8-32 MHz) to FBE, **HSRUN** (`SMC_PMPROT`
   AHSRUN, `PMCTRL` RUNM = 3), **PLL** (`C5` PRDIV 1: 8 MHz; `C6` VDIV 29: x 45 = 360 MHz VCO, / 2 = 180 MHz), `SIM_CLKDIV1`
   `0x01160000` (core /1, bus /2 = 90 MHz, FlexBus /2 = 90 MHz, flash /7 = 25.7 MHz), PEE. The dividers give the bus and the FlexBus
   the 90 MHz the SDRAM bring-up below is timed for; the arithmetic
   is in `clock_config.h` and pinned by `mk66_sdramc_config_test.cpp`. **Every wait on hardware is bounded** (about a second at
   the slowest clock); on timeout the image stops and flashes code 2 instead of hanging.
6. NVIC (`hal/irq.h`, ADR 0013 D1, mapped to MK66 lines in `irq_map.h`): grouping 0, a priority for all 100 device interrupts
   (unclaimed ones lowest, 224), MemManage/BusFault/UsageFault at 0 and **enabled**, PendSV at the render level, SysTick at 32;
   divide-by-zero traps. SysTick at 1 kHz; interrupts on.
7. **SDRAM** (`sdramc_config.h`): power key PTA24 high (already so since step 3; driven again with the same PCR, no glitch), 10 ms; FlexBus and SDRAMC clocks; `SIM_SOPT2` CLKOUT = FlexBus clock,
   FBSL = 3; `FB_CSPMCR` byte-enable groups; the **39 pins at mux 5**; 200 us for clock and CKE; controller `CTRL`/`AC0`/`CM0`;
   precharge-all, refresh on, 200 us, mode register set at `0x70001800`; then the wiring tests and (option) the full test.
   The timings (row time 15,625 ns, six-clock refresh timing, 16-bit port, command bit 20, CAS 3, 16 MiB block, 90 MHz) are
   this image's values for the board's SDRAM part; the board's address-pin wiring fixes the
   command bit and the mode word's address. **The waits are deliberately long:** this image waits 200 us for the clock
   and 200 us (> 8 x 15.5 us) for the refreshes. A longer wait cannot break a part that initialises with a shorter one.
8. Constructors, then `main`.

Step 4b (OS-PERF3, `O3OS_CODE_CACHE`, default ON): the LMEM code cache is turned on right after the C runtime, with a single store (`cache::enable()`); after the SDRAM tests, `boot::init_sdram`
also widens the controller's address mask so that the SDRAM answers at the ICode alias 0x0800_0000 (`O3OS_SDRAM_ICODE_ALIAS`, default ON). See "Caches, placement and what a block costs" below. USB, the RTC and the IRC48M are untouched.

### Pins driven or read in the first boot steps, and what this image does with each

The facts are those of the board: which pin does what, its level and its pull, on the Teensy 3.6 pin map (the Teensy core's pin
initialisation is commented out there: every pin is Hi-Z until a `pinMode`). "Driven" = a GPIO output set to its boot level in
`boot::early_pins()` (`board.h` `kQuietPins`, tested in `mk66_sdramc_config_test.cpp`): the power key PTA24 first (HIGH, `kSdramPowerPcr` `0x144`), the codec mute PTD6 LOW at once, then a 2000 µs wait (`board::kRailSettleUs`, counted on the cycle counter at the 20.97 MHz reset clock), then the other quiet pins in the table's order. Since OS-BUG-PADS step 3 the rail comes up at the start of the reset sequence instead of in the SDRAM bring-up, after the clocks and the C
runtime; the 10 ms wait of the bring-up is kept. The reason: on a later board revision (Control 4.0 / Grid 2.0) the pad chips never answered, and a quiet pin driven HIGH into a rail
that is still off can back-power it. Whether the rail also feeds the pad chips is an open research question. **This order and the PCR are untested on both units** (the maintainer's
first-batch unit and the later board revision); the first-batch unit should be checked for boot, the SDRAM tests, the display and a quiet output first (checklist MK66-10, -11).
None of the rest of this has been seen on a unit yet either.

| Function (`board.h` / `pins.h` name) | Pin (Teensy pin, port.bit) | Board fact | This image |
|---|---|---|---|
| audio mute key (`kAudioMute`) | (21, PTD6) | output **LOW = muted** (HIGH unmutes) | **driven LOW, right after the power key** (the second pin action; first before OS-BUG-PADS step 3) |
| status LED (`kStatusLed`) | (78, PTC19) | output, active low: HIGH = off | **driven HIGH** |
| input source select (`kInputSourceSelect`) | (24, PTE26) | output; **LOW routes the FM receiver, HIGH the line jack** (OS-011c) | **driven LOW**: the receiver's position, with the receiver held in reset, so silent |
| radio reset (`kRadioReset`) | (67, PTE27) | output; LOW holds the Si4703 in reset, HIGH runs it; its SDIO is the shared I2C data line (Teensy pin 48), which the chip wants LOW during reset; GPIO2 is (68, PTE28), an input with a pull-up | RST **driven LOW** (radio held in reset, silent); SDIO not driven (it is the shared I2C data line: driving it would jam the bus) |
| FT812 chip select and power-down (`kFt812ChipSelect`, `kFt812PowerDown`) | (54, PTD15), (55, PTD11) | outputs; PD# low and then high is a reset of the chip | **both driven HIGH** at boot (the first state: FT812 deselected, in standby); from OS-007c the display bring-up (see "The display") then pulses PD# and drives SPI2 on PTD12-14 |
| MIDI DIN output (`kMidiTx`) | UART4 TX (33, PTE24); RX (34, PTE25) | TX is a UART output (idles HIGH); RX is an input with a pull-up | TX **driven HIGH** as a GPIO (idle); RX not touched by the reset sequence; OS-005c2's `midi_io::start()` hands both to UART4 (section "MIDI drivers") |
| SDRAM power key (`kSdramPower`) | (80, PTA24) | output; HIGH powers the SDRAM (it may feed more than that on the later board revision: open question) | **driven HIGH, first** (`early_pins`, PCR `0x144` = GPIO + high drive + slew limit; OS-BUG-PADS step 3, untested on both units), then 2 ms (`kRailSettleUs`) before the other quiet pins; the SDRAM bring-up drives it again (same value), then waits 10 ms |
| SDRAM bus | CLKOUT, CKE, control, address, data (39 pins) | mux 5 (FlexBus) | the SDRAM bring-up |
| audio codec | SGTL5000 on I2C0 (**PTD8 SCL, PTD9 SDA**: Teensy pins 47/48; this table said PTA12/PTA13 until OS-005a, which are the grid-pad bus's pins 3/4, see "Audio"), MCLK/I2S pins on port E | no GPIO enable: the codec is clocked from MCLK and configured over I2C | not touched by the reset sequence (codec at its power-on defaults; the mute key holds the output off); `main` starts the audio path, "Audio" below |
| SD card | card detect (2, PTD0); SDHC pins | card detect is an input with a pull-up | not touched |
| encoder (`kEncoderA`, `kEncoderB`) | PTA10, PTA11 (the Teensy pins 10 and 11 are PTC4/PTC6 and unused); the push line is (41, PTD29) | `PCR = 0x612`: ALT6 = FTM2 quadrature decoder, passive filter, pull-**down** | not touched by the reset sequence; the input bring-up (OS-007d, "Input") configures the decoder pins this way; the encoder push is not configured (the encoder has no push in use) |
| pad and button scanners (`kPadsInt`, `kButtonsInt`) | (38, PTC11), (14, PTD1); two TCA8418 on I2C: pads on I2C2 (PTA12/PTA13), buttons on I2C0 (PTD8/PTD9) | INT lines are inputs with a pull-up (open drain, active low) | not touched by the reset sequence; OS-007d reads the INT levels as pull-up inputs and talks to the chips, see "Input" |
| pad LEDs | IS31FL3731 on I2C2 at 0x74 | an LED matrix driver; its registers outlive a reset of the MCU | not touched by the reset sequence (expected off from the chip's power-on shutdown state: to confirm, MK66-10); the OS-007d driver sets the chip up on its first flushes (see "Input") |
| power switch (`kPowerSwitch`) | (9, PTC3) | input | **read once a pass of the main loop since OS-POWER P1** (`power/src/power_host_mk66.cpp`; GPIO input, never driven; see "Power switch (OS-POWER)") |

Pins of the board with no role in this image, never driven: PTA8, PTA9, PTA27 and PTA28 (four multiplexer-control lines of unknown
use), the serial-flash pins and the haptic DAC. PTE27 and PTE28 are the radio's reset and GPIO2 (above).
The bootloader request pin 81 (PTA1) is driven only by the app's firmware-update request
and, since OS-006c, by `fw update` of this image (`update/src/fw_host_mk66.cpp`, the one source that names it: `check_layers.py` rule 7); nothing else touches it.

## The static image checks

`tools/check_image.py check` runs after every link (a failing HEX is **deleted**, and the build fails; it is also ctest
`mk66_image`):

| Check | Passes when |
|---|---|
| `hex_parse` | every record is well formed with a right checksum, EOF record last, no byte given twice, only record types 00, 01, 02, 04: **no start-address record** (the build passes `objcopy --set-start=0`; the bootloader's handling of one is unknown) |
| `address_range` | every populated address is <= `0x000FFFFF` |
| `head_present` | `0x0..0x40F` all populated |
| `contiguous` | one run from 0 to the last byte, **no hole** anywhere |
| `initial_sp` | the word at 0 is 8-aligned, inside SRAM, and exactly `0x20030000` |
| `reset_vector` | the word at 4 is a Thumb address into the populated image, past the flash configuration |
| `vector_table` | all 116 entries are Thumb addresses into the image, the five reserved slots (7-10, 13) are 0; with `--ramfunc-l 0x1FFF0000:0xC000` (OS-005a: the audio ISRs run from SRAM_L) an entry may instead be a Thumb address inside that 48 KiB code region, within the image's own length; anything else in SRAM, the module slab included, is refused. The reset vector must still be in flash |
| `flash_config` | bytes `0x400..0x40F` read back exactly `ffffffffffffffffffffffffdef9ffff`, FSEC `& 3 == 2` |

**Flash configuration field.** FSEC `0xDE` leaves the MCU unsecured; a wrong value can permanently lock it. Four independent
defences that share no constant: `static_assert`s in `flash_config.h`; the linker script's asserts (section at exactly
`0x400`, 16 bytes); the post-build HEX check (its own copy of the required bytes); and ctest `mk66_flashconfig` (a copy in
`CMakeLists.txt`); `mk66_flash_config_test.cpp` holds a fifth, and the flashing-safety rules the gate's. The checker's
self-test (149 cases) flips every byte of the field, plants holes in the image and every record-level defect, and requires each to fail.

## Faults and the crash record

HardFault, MemManage, BusFault, UsageFault, NMI and every interrupt without a handler enter one naked stub that finds the
stacked frame (EXC_RETURN bit 2), **masks interrupts (`cpsid i`, its first instruction, so no ISR can run on the fault stack's
account) and moves to a 512-byte fault stack** (so a stack overflow does not make the reporter fault), and calls
`o3os_fault_entry`. PRIMASK does not mask NMI or HardFault: a fault inside the reporter re-enters the stub on the same fault
stack, and the CRC protects the reader from a record written twice. A frame pointer outside SRAM or unaligned is never read. The handler writes a 64-byte
`CrashRecord` (`crash_record.h`: magic `O3CR`, version, count, exception number, EXC_RETURN, CFSR, HFSR, MMFAR, BFAR, stacked
PC, LR, xPSR, frame address, a module/stage context word, uptime, CRC-32) into the 128-byte **`.noinit` block at `0x20000000`**
and resets with SYSRESETREQ. At boot a record that validates (magic, version, size, CRC) is copied aside and offered by
`boot_crash_record()` for a future `diag crash`; `clear_crash_record()` erases it; a crash before it is cleared counts on from it
(`count` is a consecutive-crash counter for ADR 0002's quarantine). The frame layout (r0 r1 r2 r3 r12 lr pc xPSR) is pinned by a test. `set_stage()` / `set_module()` stamp where the OS is; `panic()` is the software
equivalent. A crash reset is `RCM_SRS1` SW; `boot_info().reset_sources` carries SRS0/SRS1.

Whether SRAM keeps its contents through every reset source of this chip (SYSRESETREQ is the one this image uses; a watchdog
reset and a lockup reset are the ones ADR 0002 validation 6 asks about) is a hardware check. If it does not, the same record
moves to the VBAT register file with no change to `crash_record.h`.

## Firmware info (`_fwinfo`)

ADR 0011 and `docs/hw/tracker-hardware-notes.md` describe `Firmware/_fwinfo` only as the *file* the app writes on the SD card (its content is the new
image's file name) before pulling the bootloader request pin 81 low (the bootloader is thought to delete it afterwards: not observed here, an open research question). Neither defines a version block inside the image, so
none is written. Whether the bootloader reads any version data from the flashed image (and where) is **still unknown**: a question for the maintainer and,
if needed, a test on the unit. Since OS-006c `diag fw update` writes `_fwinfo` (exactly the bare image name) and drives the pin: see
"Firmware update over USB". The images of the first sessions (before OS-006c) have no update path: see the recovery warning below.

## LED codes (the image's only output)

`PTC19`, active low (the board's status LED, `board.h` `kStatusLed`). Source and timing: `blink.h`.

| Pattern | Meaning |
|---|---|
| LED on, steady, from a fraction of a second after power-up | the SDRAM tests are running (full test: a few seconds by estimate; MK66-1 measures it) |
| 100 ms pulse every second | **heartbeat**: reset sequence finished, `main` alive. The pulse is driven from a function in SRAM_L, so it also proves `.ramfunc_l` was copied |
| 2 flashes, pause, repeat | **clock failure**: crystal did not start, HSRUN was refused, or the PLL did not lock (code `kClockFailure`) |
| 3 flashes, pause, repeat | **SDRAM failure**: the wiring or full-capacity test failed (`kSdramFailure`) |
| 4 flashes, pause, twice, then heartbeat | the FT812 did not come up (no answer, a stuck reset, no scan-out after 3 whole sequences): the image runs headless (`kDisplayFailure`, OS-007c) |
| 5 flashes, pause, once, then heartbeat | the display is up but its DMA self-test failed: everything goes by the polled path, slower and correct (`kDisplayPolled`, OS-007c) |
| 8 flashes, pause, **repeated for good in place of the heartbeat** | the display stopped swapping frames and one re-initialisation did not bring it back (`kDisplayStalled`, OS-007c): the display is held dark, the OS keeps running |
| 9 flashes, pause, **repeated for good in place of the heartbeat** | the app could not start (its bitmaps did not go up; `kAppFailure`, OS-007c): the display is held dark, the OS keeps running |
| 10 flashes, pause, **repeated for good in place of the heartbeat** | **audio failure** (OS-005a): the codec did not answer or acknowledge, the MCLK divider did not update, or no I2S boundaries came; the outputs are muted. Since OS-006b the image does not stop: it goes on without sound, USB and `diag` come up, `diag stage` lists "audio" under start-up faults and `diag log` has the "boot: audio FAILED" line with the `hal::audio::Device::Result` code (`kAudioFailure`) |
| 11 flashes, pause, repeat, from some moment on | **audio fault seen, sticky** (OS-005a): an underrun, a late render, a missed boundary, an I2S or eDMA error, or the block clock stopped (300 ms without a boundary) since boot (`kAudioFault`) |
| 12 flashes, pause, repeat, from some moment on | **over budget, sticky** (OS-005a): no fault, but a render took more than 60 % of the block (`kAudioOverBudget`); a fault replaces it |
| 13 flashes, pause, once, then heartbeat | a key scanner did not answer its configuration: the OS runs with what did, and retries the other in the background; the INPUT TEST screen says which (`kInputDegraded`, OS-007d). **When it fires** (OS-BUG-PADS): for the **control buttons** at boot, before the audio start, as before; for the **pads** not at boot but from the main loop, once the pad bus has opened (a second after the board's power key at the earliest, and after the audio start) and the pad scanner has refused its first 3 configurations since then and never answered (about 2 s after the gate opens: one try on the first pass, the others a second apart), shown in place of one heartbeat period only. The retries go on once a second for ever, and a pad scanner that answers later brings the pads online with no restart (the code is not repeated) |
| 6 flashes, twice, then heartbeat | a valid crash record from the previous run: **an unexpected crash** (`kCrashSeen`) |
| 7 flashes, twice, then heartbeat | the record is the drill's deliberate UsageFault, fields as expected: **round trip proven** (`kDrillRoundTrip`) |
| dark | the image never reached `early_pins`, or the LED is not wired as assumed |

One LED serves all the slices, so the codes are numbered apart (4, 5, 8, 9 the display's; 10, 11, 12 the audio's; 13 the input's; `mk66_blink_irq_test.cpp` pins every
value and their distinctness). When several apply to the running image, the LED shows a hard audio fault (11) first, then a display or app
safe-state code (8, 9), then the audio verdict (12 or the heartbeat); all of them are sticky, and the status words carry the rest.

## Caches, placement and what a block costs (OS-PERF3)

OS-PERF3 (rt-optimize: measure, then memory and placement, then algorithms) started from the 0.4.6 measurements: a block is 64 frames = 261,120 cycles at 180 MHz and the t-v16 song's worst render
was 82.5 % of it (91.5 % with twelve live voices), a TB-303 voice alone 130k cycles. Everything below was measured on the unit with `diag bench` and `diag cpu`; the host and QEMU suites prove the arithmetic
is unchanged, not the speed.

**What the chip has (K66 RM chapter 30, `lmem.h`).** One cache: 8 KiB, two ways, 16-byte lines, on the *code bus* only (every access below 0x2000_0000 except SRAM_L, which is routed before it). There is
**no system-bus cache**: no PSCCR (the LMEM block is five registers), `FSL_FEATURE_LMEM_HAS_SYSTEMBUS_CACHE` is 0, RM 30.4.1.2 sends system-bus accesses straight to SRAM_U or the crossbar, and `diag bench mem`
shows SDRAM at 0x7000_0000 costing the core 28 cycles a load with the cache on or off. The region modes of PCCRMR (reset 0xAA0FA000) already make flash and the SDRAM alias write-through; `ENCACHE` is the only
thing to set. What the RM offers for data in SDRAM is the ICode alias (Table 5-1, 0x0800_0000-0x0FFF_FFFF, "SDRAM (Aliased area)"): with `CM0[BAM]` bits 30:27 also "do not care" (`sdramc_config.h`
`kCm0WithAlias`, written after the memory tests) the same 16 MiB answer there, over the code bus, through the cache (measured: 10 cycles a load on a hit, the same as SRAM; `diag cache check`).

**The numbers a person needs (`diag bench mem`, cycles per 32-bit load, loop overhead of 10 included, so subtract 10):** SRAM_U 10.0, SRAM_L 10.0, flash sequential 10.0 and random 21.0 uncached / 10.0 cached,
SDRAM 28.1 through 0x7000_0000 always, 10.0 through the alias on a hit.

**Single-alias rule (`sdram_view.h`).** The cache is write-through and does not allocate on a write miss, so it is coherent as long as every core access to a byte uses one address, and nothing but the core
touches SDRAM (the SDHC is polled, the display's eDMA reads SRAM staging, I2S and USB are SRAM). The users that work in the alias are named in `sdram_view.h` (`mem::core_view()`): the sample arena and the boards
behind it, the delay and reverb memory, the project arena, the module region. Everything else (the undo journal, the log, the slack window) stays at 0x7000_0000. No object that uses LDREX/STREX lives in an alias
region. `diag cache check` is the regression test on the unit (`cache_check.h`, `bench.cpp`): a scratch round trip through both addresses (`scratch`), a negative control that shows the cache really holds lines
(words changed through the other address read stale until an invalidate: `control`), WRITE HITS (a 2 KiB block is read through the alias so that it is cached, then written through the alias by 32-bit, 16-bit and 8-bit
stores with distinct values at different offsets of each 16-byte line, two bytes of every line left alone, and read back through the alias, through 0x7000_0000 and through the alias after a full invalidate:
`hit-alias`, `hit-memory`, `hit-inval`; the last byte of each line is changed behind the cache first, which tells how many of the stores really were hits), the scratch put back (`restore`), and the checksum of the loaded
project's PCM through the alias, through 0x7000_0000 and through the alias after a full invalidate (`pcm`). One line per part, `cache check <part>: PASS|FAIL|SKIP ...`, then the line to grep: `cache check: PASS`
(or `cache check: FAIL <parts>`; `cache check: SKIP ...` when the cache is off, so a run that did not exercise it cannot pass). Hardware item: it must print `cache check: PASS` on the unit before OS-PERF3 ships.

**Placement.** The module instances' state (`module::ModuleSet::state()`, 4 KiB of SRAM_U, falling back to the SDRAM arena) and the module voices (`g_module_voices`) are in SRAM_U: the TB-303's state is read
and written about a hundred times a sample. SRAM_U headroom over the 32 KiB floor went from 12.5 KB to about 7 KB. The SRAM_L split moved 1 KiB from the module slab to the hot code (hot budget 27 KiB, slab 14 KiB;
a module whose hot part does not fit runs from the SDRAM, which the alias caches). The send effects' and the limiter's state and coefficients were already in SRAM_U.

**Tools.** `diag bench <kernel|all> [blocks] [alias|direct|sram]` (`bench.cpp`, `bench_report.h`; only in an image built with `-DO3OS_BENCH=ON`, OS-BENCHOPT): the DWT cycles of one block of the sampler voice, a TB-303 instance, the delay, the reverb (HALF and FULL), the
limiter (loud and quiet), `strips`/`stages` in situ, `mem`, and `worst` (the slowest render since `cpu reset`, split by stage). `burst` (OS-ODD11 UTAIL) runs 12 filtered voices on the live pool (stop the transport, mute nothing): `burst_on` (the 12 note_ons of a step), `burst_first` (the block after, 2 spans), `burst_steady` (1 span), `burst_4span` (4 spans: the per-span cost of the voice loop); it calls nothing of the gate, so it builds on 2e40e69 for the A/B. `cpuopt` (OS-CPUOPT1) runs the loop player's block and the SVF (three types) for the old reference and the production code, one line each, plus `BENCH cpuopt_eq_* OK|FAIL` lines that compare their outputs on the unit. `looper` (OS-LOOPER L2b) runs the overdub kernels on SDRAM buffers, cache-cold: `rmw_fb100`, `rmw_fb50` (each aligned and `_odd`), `cow_copy`, `restore_step`, plus `BENCH looper_eq_rmw|restore OK|FAIL`; its numbers fix `kRestoreCycles` and the L2 gate (spec 3c.7). One kernel call at a time runs with the render held off by BASEPRI = 128, so the
audio keeps flowing and the call is never preempted by the render; the reported figure is the minimum over the run (avg and max after it). `diag cpu reset` zeroes the maxima and the fault counters (the fault LED's latch, `fault_seen`, is kept: the LED stays on) and turns the module instruments' own measure ON (OS-303PERF, below); `diag cpu perf off` turns it off.
`diag cache [on|off|inv|check]` switches the code cache for an A/B at run time.
The kernels that make a signal of their own (`tb303`, `voice`, `voice_f`, `voice_od`, `voice_bit`, `voice_odbit`) also LOOK at it (OS-MODSILENT, `bench_report.h` `Peak`, outside the timed part): the result line ends `(<note> peak N)` (of 32768 full scale), and an
output of zeros or with a NaN adds `BENCH <kernel> FAIL: the output is silent ...`. A bench that prints a cycle count has run; one that prints a non-zero peak has also made a sound, which is what `tb303` checks on the live instance
(the bench runs a private instance, init()ed afresh each run, in the memory the live one's state is in: SRAM_U since OS-PERF3; its cold part is initialised through the alias; the figure is the same run after run). `mod note` plays on track 16, the spare strip: `Engine::attach_modules()` switches it on (the graph's default mask is the twelve tracks of a new project).

**OS-IFX1 (overdrive and bit depth of a sampler voice).** With the transport stopped: `diag bench voice 64`, `diag bench voice_od 64`, `diag bench voice_bit 64`, `diag bench voice_odbit 64`: the same sample and note, with the overdrive at 100, the depth at 8 bits, or both on a copy of the descriptor; the cost of the two passes is the difference of the minimum to `voice` (estimate from the disassembly, not measured: about 17 cycles a sample for the overdrive, 8 for the depth, 20 for both, so roughly 1100, 500 and 1300 cycles a block, 0.2 to 0.5 % of a 261k block). OFF is the `voice` line itself (the pass is skipped). **OS-PERF-CPU2:** `diag bench voice_rev 64` is the same voice played backward (the PLAYBACK layer on track 16, taken away at the end); the difference of the minimum to `voice` is the cost of the backward loop. `diag bench voice_pan 64` is the same voice with an instrument pan LFO (a saw of 8 steps, amount 0.8) on a copy of the descriptor; the difference to `voice` is the voice-side cost of the LFO (`Engine::apply_pan_mods` is not in the pool-only line: `diag bench worst` splits the strips).

**The module instruments' own measure (OS-303PERF, `audio/module_perf.h`).** `diag mod` gives each instance a line `mod: instrument N: ... , state SRAM|SDRAM, hot code slab|SDRAM` (where it runs from) and, while the measure is ON, a second one,
`mod: perf N: <blocks> blocks, loop min a mean b max c, render min d mean e max f, out mean g, hooks h` (cycles of the 180 MHz counter, full blocks only; `no full block measured` until the first). `render` is `in.mod->render()` alone (the live twin of `diag bench tb303`), `loop` the whole
instance loop of the render (zero-fill, enter, render, leave, Q15 conversion: the governor's figure plus the zero-fill), `out` the zero-fill and the conversion, `hooks` the rest (the crash-context stamps). The header line of `diag mod` says `perf on` or `perf off`; the refused modules
(`mod: quarantined ...`) are listed first, the listing stops at 24 lines and says so when it cut. **On and off.** The measure is OFF from boot: until it is turned on the render reads one flag a segment, about 10 instructions an instance and block. `diag cpu reset` turns it ON (and starts its figures again with the engine's maxima: every
HIL scenario that begins with `cpu reset` leaves it on, which is why there is an off); `diag cpu perf off` turns it OFF (no perf line is listed while it is off: read `diag mod` first). **What it costs while it is on, and what it does to the governor's figure.** Three counter reads and three adds a segment and one fold a block
(flash): about 150 instructions an instance and block, 0.1 % of the block [est: QEMU icount, `module_governor_test`; not measured on the unit]; nothing is printed or locked in the render. The three reads (after enter(), after render(), after leave()) fall inside the window of the governor's own stamps (`t4 - t0` of a segment), so while the measure is on the governor's figure
(`cycles/voice` of the instrument line, the cost it admits notes by, and the engine's render time in `diag cpu`) is about three counter reads a segment higher than with it off. That is small against a 261,000-cycle block, but a figure that is to be compared across builds, or the baseline of a CPU study, is taken with the measure OFF.
`diag bench tb303_128` asks the same private instance for 128 frames in one call (`O3M_BLOCK_MAX`), `tb303` for 64: the note carries cycles per frame.

**Protocol for the unit** (USB off = the unit presents STANDARD, so no isochronous interrupt lands in a figure; the console stays on the CDC serial port): Config > USB, USB AUDIO Off, F7 twice, reconnect the terminal; `diag usb mode` should say STANDARD (the `qa_global` scenario checks it softly and records the mode: in AUDIO it still
captures the placement lines, but its maxima carry the isochronous interrupts and are not a baseline; say so in the report); `diag mod load tb303`, `diag mod bind 61 tb303`. The held note (`diag mod note 61 36 110`) sounds about 17 s [host run of the voice: 11,700 blocks] before the instance is idle, and an idle instance is not rendered, so a held note is measured for **8 s**
(about 5,500 blocks; `qa_global` does exactly this) and a song with the TB-303 in it for **60 s** (the reference project of `docs/architecture/cpu-budget.md` section 5).
(1) With the measure ON: `diag cpu reset`; play; then `diag mod` (the perf line), `diag cpu`, `diag bench tb303 64`, `diag bench tb303_128 64`; then `diag cpu perf off`.
(2) The OFF baseline of the same run, to see what the measure adds to the governor's figure: `diag cpu reset`, then at once `diag cpu perf off` (the reset zeroes the maxima and turns the measure on; the off turns it off for the run), the same song or note, then `diag cpu` and the instrument line of `diag mod` (`cycles/voice`).
The difference of the two is the measure's own cost. The hil scenario `qa_global` does (1) for the held note, prints the replies (`"show": true` steps) and ends with `cpu perf off`.

## Audio (OS-005a): I2S0, eDMA, the SGTL5000, the render interrupt

ADR 0013 D1/D2/D7, ADR 0012 amendment 2, ADR 0011, `docs/specs/OS-005-audio-engine.md` 005a. Written from the K66 Sub-Family Reference
Manual's register facts and the SGTL5000 data sheet (Freescale/NXP, the revision PJRC hosts; read for this slice). **Nothing here has run on
hardware**: the pure parts are unit-tested on the host and on the Cortex-M4F, the fake I2S proves the protocol on QEMU, and the rest is
the HW-A checklist below.

### Order of events (`src/audio.cpp`, `hal::audio::Device::start()`; H16)

1. The amplifier key (PTD6) is driven LOW, whatever it was, and both DMA rings are zeroed (digital silence).
2. Clock gates; I2S0 reset; the master clock: `I2S0_MCR` = MOE (system clock), then `I2S0_MDR` (each wait on DUF bounded), then the
   MCLK pin (PTE6, mux 4): the codec never sees a clock of the wrong frequency. 5 ms settle before the first I2C write.
3. I2C0 (bus released if a slave holds SDA), the codec's part id (`CHIP_ID` must read `0xA0xx`), then the **power-up plan**
   (`sgtl5000.h`): every output muted and every volume at its floor first, then the supplies, then the analog outputs powered
   (the VAG ramp starts), a 500 ms wait, then clocks and routing. The plan ends with line out and headphone **still muted**.
4. I2S transmit/receive configured (not enabled); the eDMA channels' TCDs loaded and armed (the eDMA's global configuration, `DMA_CR` and
   every priority register, is NOT written here: `boot::dma_init()` wrote it once in the reset sequence, from the table in `resources.h`,
   and `start()` panics if that has not happened); the pipeline initialised; the two interrupt lines enabled.
5. Pins for the frame clocks and data; receiver enabled, then transmitter (the receiver is synchronous to it): clocks and DMA start,
   the DMA feeds silence.
6. A bounded wait for two boundaries (25 ms): if none comes the hardware is stopped and `start()` fails, still muted.

`start()` never unmutes. `unmute()` (the `O3OS_AUDIO_TEST` image calls it after 400 clean blocks) writes the plan of `unmute_step()`:
headphone volume at the floor, DAC volume 0 dB (the DAC is still muted), the DAC unmuted through its **soft ramp** (50 ms wait), line out
and headphone opened (the headphone at its minimum), the headphone volume stepped up one 0.5 dB step per 2 ms to code 0x40 (-20 dB),
and **last** the amplifier key HIGH. `mute()` is the reverse: key LOW first, then the codec. The properties are tests, over a model of
the codec's registers (`mk66_sgtl5000_test.cpp`): outputs and DAC muted at every step of the power-up, headphone at its floor before the
headphone is powered, never above -12 dB (code 0x30) at any step, the ramp in single steps, MCLK inside 8-27 MHz and MCLK / 256 within 1 %
of 44.1 kHz for both rate constants.

The power-up plan is built for a quiet start, in the direction of quieter and earlier: volumes and mutes first, before the ramp; the
part id read; the line out stays muted until `unmute()`; a 500 ms wait (the VAG ramp is about 400 ms with SMALL_POP). The register values fit
this board (VDDIO = VDDA = 3.3 V, VDDD external).

### Settings and where they come from

| What | Value | Source |
|---|---|---|
| I2S0 pins | MCLK PTE6, RXD0 PTE7, TXD0 PTE10, LRCLK (TX_FS) PTE11, BCLK PTE12, all mux 4 | the board's wiring; NXP's device header |
| Codec bus | I2C0, SCL PTD8, SDA PTD9, mux 2, open drain, high drive, slew limited, external pull-ups; address 0x0A; F = 0x1F (divide 240: 90 MHz / 240 = 375 kHz, data sheet maximum 400 kHz) | the board's wiring; NXP's device header |
| MCLK | `I2S0_MCR` = MOE, MICS 0 (system clock 180 MHz); `I2S0_MDR` FRACT 15, DIVIDE 254 = 16/255: **11,294,117 Hz = 256 x 44,117.6 Hz**. After D-OS-8 FRACT 195, DIVIDE 3124 = 196/3125: 11,289,600 Hz = 256 x 44,100 Hz exactly. Both follow from the one rate constant `usb::kRefCyclesPerSample` through `hal::audio::mclk_ratio()`; both are inside the codec's synchronous 256 Fs range and 8-27 MHz | NXP's device header |
| Frame | I2S master; two 32-bit words, MSB first; frame sync one bit early, low = left; BCLK = MCLK / 4 = 64 Fs; a sample is the upper half of its word (16-bit DMA to/from data register + 2). TCR1 1 (watermark 1: the DMA request is raised with one word left, so the margin before an underrun is about 2 word times, **11-22 us**; TFW = 4 would give 45-57 us for 0.05 ms more latency but leaves the value proven on this board, and the chip's FIFO depth is not stated in the repository, so the choice is 1 and the real margin is HW-A row A-6), TCR2 0x07000001, TCR3 0x10000, TCR4 0x00011F1B, TCR5 0x1F1F1F00; receiver RCR1 1, RCR2 0x47000001, synchronous to the transmitter, otherwise the same | NXP's device header; values checked in `mk66_audio_config_test.cpp` |
| Codec I2S word | `CHIP_I2S_CTRL` 0x0130: slave, 16-bit, I2S, SCLKFREQ 32 Fs, the value proven on this board, although BCLK is 64 Fs; the data sheet's letter for a 64 Fs slave is 0x0030, an A/B item (E-1) | data sheet `CHIP_I2S_CTRL` bit 8; values checked in `mk66_audio_config_test.cpp` |
| eDMA | `DMA_CR` = EMLM \| EDBG, written with every `DCHPRI` by `boot::dma_init()` only; channel **13** transmit (DMAMUX source 13), channel **12** receive (DMAMUX source 12; the channel numbers and the request-source numbers are different things that happen to coincide); 2-byte minor loop, major loop one slot (128 transfers), the TX channel interrupts at every major loop (one interrupt per 1.45 ms block); `ATTR[SMOD]` / `[DMOD]` = log2(ring bytes) so the address wraps inside the ring; no scatter/gather, no DREQ, SLAST 0 | NXP's device header |
| Rings | TX 3 slots in a 4-slot ring = 1 KiB, `alignas(1024)` (2 slots = 512 B before OS-CPU10 slice 3); RX 4 slots = 1 KiB, `alignas(1024)`; in SRAM_U, section `.dma_rings`; a slot is 64 stereo int16 frames = 256 B | ADR 0013 D1, D7; `audio.cpp` |
| Arbitration | one table, `resources.h`, written by `boot::dma_init()` (HALT set while the registers are rewritten, every flag cleared) and by nothing else; audio highest (TX 15, RX 14), the display's channels next (14 gets 13, 15 gets 12), the free channels below with their own numbers: a permutation of 0..15. The eDMA arbitrates at minor-loop boundaries, so what the display can cost an audio request is one of its minor loops: **the display's are at most 4 bytes** (`edma_config.h`, a `static_assert` and a test), a few bus cycles, against the 11-22 us FIFO margin | `resources.h`, `boot.cpp` |

**The DMA can never write outside its rings**: the receive TCD's destination is `[ring, ring + 1 KiB)` by modulo, aligned by declaration
(`alignas` + `static_assert`, the section's alignment, a runtime check in `start()` that panics on a mismatch), and
`mk66_audio_config_test.cpp` steps a model of the eDMA's address arithmetic through five trips round each ring (and shows that an
unaligned ring *would* escape, so the model is not vacuous).

### Interrupts and the channel-ownership table (H11)

| Prio | Line | Work |
|---|---|---|
| 0 | MemManage, BusFault, UsageFault | faults |
| 16 | eDMA channel 13 (IRQ 13), `DMA13_DMA29_IRQHandler` in SRAM_L | the slot boundary: stamp `CYCCNT`, clear the request, read the TX/RX DMA positions and the error flags, number the boundary, check the block due now, publish, **pend the render**; no audio data touched but a stale slot zeroed after an underrun mute (64 word stores, about 1.1 us [est], only on that path; it stays in the ISR because when the mute matters the render is the context that has stalled) |
| 32 | SysTick | 1 kHz time base |
| 64 | USB0 (IRQ 53), `USB0_IRQHandler` (flash) | OS-006b: moves packets between the buffer descriptors and the rings, nothing else; bounded, see "USB and the `diag` console" |
| 48 | UART4 RX (IRQ 66), `UART4_RX_TX_IRQHandler` in SRAM_L | OS-005c2: stamp every received DIN byte with `CYCCNT` and push it into the DIN `midiRt` ring; bounded (4 bytes), see "MIDI drivers" |
| 80 | completions (SDHC, SPI2, I2C, DMA15) | flags only |
| 96 | PIT0 (IRQ 48), `PIT0_IRQHandler` in SRAM_L | OS-005c2: the MIDI-out emitter: one poll of the DinEmitter, at most one byte written to UART4, the one-shot timer re-armed; also entered by the render's software pend |
| 128 | IRQ 64 (the K66's software interrupt, `SWI_IRQHandler` in SRAM_L) | the render: copy the newest complete RX block, run the engine, commit to the TX slot if still on time |
| 160, 224 | pins, unassigned | the input scan (OS-007d) is a pass of the main loop: **no pin interrupt, no timer, no priority is used** (see "Input"); the INT lines are read as levels |

`resources.h` names the owner of every eDMA channel (13 I2S0 TX, 12 I2S0 RX, **14 and 15 DSPI2 TX/RX for the FT812 display** (OS-007c, claimed: DMAMUX sources 39 and 38, interrupt line `DMA15_DMA31` at the completion priority 80 (`irq_map.h`)), the rest free), of the
four PIT timers (PIT0 is the MIDI-out emitter's of ADR 0013 D5, **claimed by OS-005c2**; three free: the input scan claims none) and of I2S0 (the audio engine
alone); the I2C masters' clients are listed in `i2c_config.h` (`kClients`, a `static_assert` refuses two chips at one address on one bus); `static_assert`s refuse a channel with two owners, a non-permutation of the arbitration priorities, and a owner whose
interrupt line is not at its role's priority.

### What keeps the hot path in SRAM_L

`O3OS_AUDIO_HOT` (and `O3OS_RAMFUNC_L` on the ISRs) puts the ISR, `Pipeline::on_boundary/render`, `Engine::render_block`, the tone and
the status writers into `.ramfunc_l`: 2.0 KiB at OS-005a; with the graph's kernels, strips, buses, meters and taps (OS-005b) 14.6 KB (14,588 B) of the 24 KiB budget (10,388 B before the review fixes: the strip kernels are specialised for one or two tracks, stereo or centre, steady or ramping). SRAM_L is 512 MiB of address space away from flash, where a `BL` cannot reach: the linker
would quietly insert a veneer for every call that leaves the section. `tools/check_ramfunc.py` (post-link and ctest `mk66_ramfunc`, with a
self-test) disassembles `.ramfunc_l` and fails on any direct branch out of it or through a veneer; indirect calls (the cycle counter, the
render callback) are allowed because they are visible in the source. On its first run this check found
three `memset` calls (a zeroing loop and two aggregate initialisers) and two status helpers that had not been inlined: fixed
(`-fno-tree-loop-distribute-patterns` on the audio library, explicit field stores, `always_inline` on the helpers).

### Reading the status before there is a console

The LED gives the verdict (codes 10, 11, 12 above). The numbers (`diag cpu`: stage cycles, jitter, the boundary period) are in
`AudioStatus`; until the CDC `diag` exists (OS-006b/007) they can be read only through a debug probe: the 38 32-bit words at the address
held in `o3os_audio_status_ptr` (`arm-none-eabi-nm o3os.elf | grep o3os_audio`), in the order documented at the end of
`audio/include/o3os/audio/status.h` (ISR group first: boundaries, underruns, current run, max run, mute events, muted, slips, hw
faults, period min, period max; then generation, renders, late drops, governor over, render last, render max, latency last, latency max,
9 stage-last, 9 stage-max, input peak; then the fault latch (1 once an underrun, late drop, slip or hardware fault was seen, kept by `diag cpu reset`); the nine stages are input, drain, render, strips, buses, master, convert, taps, commit: `hal/audio.h` kStage*). Cycle figures are DWT cycles at 180 MHz; the block is 261,120 cycles. Whether the OG's board
exposes a debug port is not known (open question 5 below).

`diag cpu` ends with the main-loop guard's line (OS-MAINGUARD): `guard  shed N blocks in M runs  late L  longest run since boot K blocks  n W`. A render over 60 % of the block that lets the main loop finish no pass in W blocks (the window, default 64, about 93 ms; `guard n <1..255>` sets it at run time for a tuning sweep, `guard` alone prints the line; not saved) sheds (a faded silent block, the callback not called) until a pass completes; N, M and L are since boot or `cpu reset` (the render is the only writer and zeroes them at its next block after the reset), K is since boot and stops at 255. **Expect `shed 0 blocks`** with any project the unit plays comfortably (the HIL gates `cpu_play`, `cpu_mute_ab` and `stress_voices` check it); under Heavy a few runs are the guard working: K is then the main pass's cost in blocks. `late` counts shed blocks whose slot was already playing (also an underrun). A shed block leaves a hole of 64 frames in the stems tap and the recorder.

### Image sizes (cycles and bytes are measured by the build, not by hardware)

Baseline OS-007b image: 4,108 bytes of flash, 24 B SRAM_L, 744 B SRAM_U. With OS-005a: default **10,140 bytes** (+6,032), test variant
**10,188** (+48 for the unmute and tone branch); SRAM_L 1,992 B hot code; SRAM_U 7,104 B: 128 + 512 (crash block, fault stack), 384
alignment padding, 1,536 DMA rings, 24 `.data`, 4,520 `.bss` (the engine 3,680 B with its 256-entry ParamChange ring and two tone
generators, the pipeline 728 B). Both images pass the eight static checks and are bit-identical when built twice in fresh build directories.

**OS-005b (the graph).** `.text` + the `.ramfunc_l` load image: default **137,536 B** (+12,036 against 125,500 at the baseline of this slice), test variant 138,180 (+644 for the unmute, the tone and the sweep script); `.ramfunc_l` **10,388 B** of its 24 KiB (the kernels, strips, buses, meters, taps and the status writers; the only calls it makes into flash are indirect, through the pointers to `usb::interleave` and `usb::sum_slots`, which `tools/check_ramfunc.py` permits); `.data` 184 B (unchanged); `.bss` 85,480 B (+32,288: the engine's statics are 36.3 KB, the old engine was 3.7 KB; `audio.cpp` caps it at 40 KiB); no SDRAM. Free SRAM_U stays above 70 KB of the 32 KB wanted. The default image, the test image (B) and its retune (C), drill (D) and stall (F) variants all build with 0 warnings and pass the eleven static checks; the default and B images are bit-identical when built twice in fresh directories.

**OS-005b review fixes** (same default image, against the first OS-005b commit). Flash **142,300 B** (+4,768; `.text` 127,688 and the `.ramfunc_l` load image 14,588); SRAM_U **64,360 B** (-22,016: `.bss` 61,288, the engine is 13,848 B instead of 36,280: the stem pool, the meters and the scope and spectrum rings left it, and one pair of shared scratch blocks replaced a block per track), **107,672 B free** after the 24 KiB stack (85,656 B before); SRAM_L **36,860 B**: `.ramfunc_l` 14,588 B (9,988 B of its 24 KiB budget free) and `.tapmem_l` 22,272 B (`TapMem`: the stem pool and ring 14.5 KB, the meters 2.4 KB, the scope and spectrum rings 2 KB each; 2,304 B of its 24 KiB budget free), 12,292 B of the 48 KiB region free below the slab. No SDRAM.

## The display (OS-007c, `display/`)

`main` brings up the FT812 before its loop and the OS app (`app::App`: the pattern view over demo data, the Amiga theme, a fixed
16 ms tick from SysTick) draws on it, through the same `eve` display lists the simulator checks (the host test `display app`
renders what the chip holds with the simulator's rasteriser and compares it bit for bit with the simulator's own run, in all
three themes). There is no input yet (a null source, OS-007d). The build option `-DO3OS_DEMO_CYCLE=ON` walks the cursor down
the pattern every 1.5 s and cycles the three themes (every fifth step), so a hardware session sees the screen move;
`-DO3OS_DISPLAY_ROTATE=0..7` sets `REG_ROTATE` (default 1, see "Orientation").

| File (`display/`) | What |
|---|---|
| `include/.../ft812_regs.h` | the FT812 facts: memory map, registers, host commands, the SPI frame layout. Cross-checked against the pinned Riedel header by the host test `eve_riedel` |
| `include/.../panel.h` | the 800x480 panel's timing and the orientation as `constexpr` values; the register list the init writes is a pure function of them |
| `include/.../dspi_config.h` | DSPI2: pins, mode, the two clock classes (init 1.125 MHz, data 18 MHz at the 90 MHz bus), CTAR values, the limits as `static_assert`s |
| `include/.../edma_config.h` | eDMA channels, DMAMUX sources and the transfer descriptors (TCDs) as pure values |
| `include/.../link.h` | the two seams: `Link` (the byte-level wire) and `BulkEngine` (the DMA path), so everything above runs against fakes |
| `chip.h`, `src/chip.cpp` | register access, the bring-up sequence, the coprocessor command FIFO and its fault recovery (pure, tested) |
| `display.h`, `src/display.cpp` | `hal::Display` on the FT812: frames, uploads, staging, swap, backlight, the DMA self-test (pure, tested) |
| `device.h`, `src/device.cpp`, `src/dev_regs.h` | the MK66 half: DSPI2 as the `Link`, the eDMA as the `BulkEngine`, the completion interrupt, `bring_up()` (hardware only) |
| `../src/app_main.cpp` | the composition root: the one platform file allowed to include the app and ui layers (`check_layers.py` lists it by name) |

**Pins** (board facts, cited in `board.h` and `dspi_config.h`): DSPI2 SCK PTD12 (Teensy pin 53), MOSI PTD13
(52), MISO PTD14 (51), pin mux ALT2 (the Teensy core's SPI2 pin map); chip select PTD15 and PD# PTD11 are GPIOs, already driven high by `boot::early_pins`.
Mode 0, MSB first, 8-bit frames. PTD10 (a reset line of the
display header) is not driven.

**Clocks.** Until the FT812's clock is up the bus runs at 90 MHz / 5 / 16 = **1.125 MHz** (the limit is
11 MHz, Guide p.11); after the bring-up proved the scan-out, **18 MHz** = 90 MHz / 5 x 2 / 2 (a divider combination below 20 MHz;
the spec's limit is 20 MHz, the chip's 30). Both are `static_assert`ed against the real bus clock and checked by a
test that also enumerates every divider combination. DBR with the /5 prescaler is a 40/60 duty cycle (about 22 ns narrowest):
a scope item (DSP-6).

**DMA.** eDMA channels **14 (SPI2 transmit, DMAMUX 39) and 15 (SPI2 receive, DMAMUX 38)**; completion is the receive channel's
major-loop interrupt on line `DMA15_DMA31` at the Completion priority 80 (ADR 0013 D1, `irq_map.h`); the handler only releases
chip select and publishes the result. The driver writes **neither `DMA_CR` nor any `DCHPRI`**: the eDMA's global configuration
belongs to one boot function, `boot::dma_init()` (`src/boot.cpp`, called once from the reset sequence before `main`, so before both
the display and the audio engine start): it writes `DMA_CR` and every `DCHPRI` from the table in `resources.h` (audio channels 13/12
highest with 15 and 14, these two next with 13 and 12, the free channels below). The engine programs only its own channels'
descriptors, the DMAMUX and its interrupt line, and clears only its own channels' error flags; `tools/check_dma_owner.py` (ctest
`mk66_dma_owner`) fails if any file but `boot.cpp` names `DMA_CR` or a `DCHPRI`. **Every minor loop is at most 4 bytes** (TX 1, RX 4; `edma_config.h` `kMaxMinorLoopBytes`, a
`static_assert` and a test): the audio engine's FIFO margin depends on a display request never holding the eDMA longer than one
4-byte beat. Each transfer is one chip-select cycle of `[3-byte write header][data]` from a staging buffer: TX moves bytes into the
low byte of `SPI2_PUSHR`, RX reads every answer frame into a one-word sink and counts the transfer out. A transfer is **bounded by
its own buffer**: the display tells the engine how much room is left behind the source (8,195 B for the frame buffer, 4,099 B for the
smaller upload buffer, never the frame buffer's for both), the engine also refuses a source not wholly inside SRAM or a count the
descriptor cannot hold, and `static_assert`s end a full frame and a full upload chunk exactly at their buffer's end. The clock gates
the hardware half opens (SPI2, DMAMUX, and the eDMA at `SIM_SCGC7` **bit 1**, bit 8 being the Kinetis-L position) are pinned by a
test against a table cited from the Teensy core's `kinetis.h` and NXP's `MK66F18.h` (`mk66_display_soc_test.cpp`). The flush that precedes each
transfer clears the DSPI's stale flags but **not TFFF** (the transmit FIFO fill request the DMA waits for).

**The open hardware question of the byte-wide PUSHR write.** The command half of `PUSHR` (the upper 16 bits) is meant to stay
as the last full 32-bit write left it (all zero) while the DMA writes only the data byte; that is the K66 behaviour the Teensy
SPI DMA relies on, but this tree has not seen it on a unit. So the display runs a **self-test** after the bring-up: 64 bytes of a
mixed pattern (including 0x00, 0xFF, 0x80, 0x7F) go to the last 64 bytes of RAM_G by DMA and are read back by the polled path.
If they differ, or the DMA does not complete, **every transfer goes by the polled path from then on** (slower, CPU-blocking,
correct) and the image plays LED code 5 once. Any DMA transfer that later fails or hangs (50 ms) is sent again by the polled
path and switches the path off the same way.

**Init sequence** (`chip.cpp`, the order of Rudolph Riedel's `EVE_init()` plus the ADR 0011 lesson-3 additions; every difference
is listed and tested in `tests/eve_riedel_test.cpp`): SPI at the init speed; **probe** REG_ID (0x7C = the chip is already
running, a warm start: REG_CLOCK and REG_FRAMES are kept, the backlight goes off at once); before that, **at least 20 ms of
uptime** after `early_pins` raised PD# (Guide p.11; a wait only if the uptime is below it); **PD# low 20 ms, high, 40 ms**; the
reset host command (RST_PULSE, for a chip whose PD# is not wired); **CLKINT on a warm chip** (so a CLKEXT that survived an unwired
PD# does not; a cold chip is sent no clock command); ACTIVE and 60 ms; poll REG_ID up to 400 ms and, **the moment it
answers, REG_PWM_DUTY = 0 and the REG_PWM_HZ if one is set, the first register writes of all** (the chip's duty resets to full and has
been counting since ACTIVE: the 60 ms wait is a **remaining window of up to about 60 ms** in which the backlight may be at its
reset duty, if the FT812's BACKLIGHT pin is driven at all before the first write; DSP-1 looks for it); poll REG_CPURESET
up to 300 ms (bounded); the coprocessor's REG_CMD_READ / WRITE must be zero (else the
Guide's recovery, once); **REG_PCLK 0 before the timing is rewritten**; the 16 panel writes (`panel.h`);
audio off; a first black list and its swap; DISP; **REG_PCLK last**; then **prove the scan-out** (REG_FRAMES advances twice and
REG_DLSWAP reads 0) and only then the data speed. A failure is retried as a whole (up to 3 sequences, each with its own PD#
pulse); a display that does not come up is LED code 4 and a headless run (the heartbeat still shows the image alive). Worst case
about 0.5 s a sequence (chip absent), about 0.2 s healthy. No CLKEXT/CLKINT is sent to a cold chip (the chip's reset default is
its internal oscillator); `HostClock` makes it a one-line change if the bench says otherwise.

**Frames and double buffering** (`display.h`): the FT812's RAM_DL is 8 KiB (2048 words) swapped in by `REG_DLSWAP = 2`
(DLSWAP_FRAME: at the next frame start, no tearing; the register reads 0 once the list memory is safe to write again). That is the
chip's own double buffer and it is the scheme used; the coprocessor (`CMD_APPEND`) is not in the frame path, so a coprocessor fault
cannot freeze the UI. **Word budget: 2048 words per frame including the closing DISPLAY** (= `eve::kMaxDisplayListWords`, tested);
the pattern grid's worst case is about 800. `write()` copies the words into a 4 + 8192-byte frame buffer (the caller's list is
free at once); `end_frame()` starts one DMA transfer to RAM_DL and, when it completes, `service()` writes REG_DLSWAP;
`begin_frame()` says false while a transfer is in flight or the swap is pending (the app retries next pass), so frames pace at the
panel's own 54 Hz at most. Uploads use a second 4 + 4096-byte buffer (the contract allows an upload inside an open frame): an
`upload()` copies, waits (bounded, longer than a hung transfer's timeout) for the transfer in front of it, and returns once its
own is started; uploads and frames reach the wire in call order. Both buffers are `alignas(8)` with the header at offset 1 so the
payload starts on a word (ADR 0011 lesson 1; a `static_assert` on the class, a run-time check in `start()`). The DMA never meets
a polled access on the bus: the display keeps its polled register accesses out of the transfer-in-flight state (a test with a bus
monitor in the fake proves it) and the MK66 link refuses them while the engine is busy.

**Swap stall.** A swap completes within one frame time (18.5 ms). If REG_DLSWAP is still non-zero after **500 ms** the scan-out is
dead: the driver **re-initialises the chip once** (its backlight goes off, its RAM_G is presumed lost: frames are refused until the
bitmaps are uploaded again, and `appimg::pass()` does so and starts the app over). If the swap stalls again right after, or the
re-init fails, the display is **given up**: `failed()`, held dark, every frame and upload refused, and `pass()` returns
`kDisplayStalled`. Tested with a chip model whose REG_DLSWAP never clears (forever, or until it is reset).

**Safe state.** One for every fault found after boot (`kDisplayStalled`) and for an app that cannot start (`kAppFailure`: the
bitmaps did not go up): the backlight is off and held off for good (`Ft812Display::hold_dark()`), the OS keeps running, and the
status LED shows the code **in place of the heartbeat for ever** (8 or 9 flashes, `blink.h`): never a silently lit black screen. A
display that never came up at all stays code 4, played twice, then the heartbeat (there is no screen to hold).

**Backlight.** It stays off until the first frame the app committed is on screen (REG_DLSWAP back to 0) and then ramps in steps of
8 every 8 ms to 127 of 128, about 130 ms; if the app draws nothing 2 s after the
display came up the ramp starts anyway (a dark screen would be mistaken for a hang). REG_PWM_HZ is not written (the chip's 250 Hz):
see DSP-10.

**Boot screen (OS-003f).** The first frame the driver is given is the boot screen (`ui/boot_screen.h`): the maintainer's logo, 124 x 124 L4
(7,688 B of flash, `ui/src/boot_logo_data.cpp`) scaled by 3 with `BITMAP_TRANSFORM_A/E` = 85, exactly centred on black, and under it
`Track3r  o3os <build id>` in the Micro face (the id is the one `diag version` prints). `appimg::start_app()` uploads the assets (the logo to RAM_G
0x6F000, `ui/plot/ram_g_map.h`), commits the boot frame before the app is built, and starts a `core::BootHold`; since the backlight waits for the
first committed frame to be on screen, **nothing is lit before the logo**. The audio start blocks `main()` for about 0.6 s (its codec waits are
`boot::delay_ms()`; the USB start does not wait with it): those waits call an idle hook once a millisecond (`IdleHook`, `idle_hook.h`, installed by
`app_main.cpp` through `boot::set_idle_hook`, never entered from itself, main thread only) that services the display, so the swap and the ramp happen
under them instead of waiting for the loop. The driver is not re-entrant (`service()` can find a stalled swap, re-initialise the chip and wait with
`delay_ms()`, which calls the hook): every entry of the main loop into it (`pass()`, the app's frames, the asset upload) sits in a `BootGate::DriverScope`
and the hook does nothing inside one. `BootGate` (`boot_gate.h`) holds those rules and decides the hold; `tests/mk66_boot_gate_test.cpp` runs it against the
real driver on its chip model (the hook, the hold, the release on a display recovery, the stall recovery that waits inside the driver). `pass()` holds the
app's first frame back until the logo has been lit for 800 ms (from the first light, the ramp's first step; a display that never lights releases it 3.3 s
after the commit), and not a millisecond longer: boot work that outlasts the minimum is not extended by it, the first pass of the loop draws the app at
once. A boot frame the display refuses (50 tries) is not fatal: the app starts without a logo. The recovery after a display re-initialisation skips the
logo and releases the hold. The LED code for a failed DMA self-test (`kDisplayPolled`, about 3.6 s) is played **after** `appimg::init()`: played before, it
outlasts the backlight's own 2 s fallback and the ramp would start on a black screen before the logo is committed. **Cost:**
flash +8,628 B (logo 7,688, code about 940: boot screen, hold, hook, transform words), SRAM_U +32 B. [A] `A = 85` is a third to 0.4 %: of the 124 blocks
one is 4 px wide and the last is 2 px, where depends on how the chip rounds (DSP-15).

**Orientation.** `REG_ROTATE` alone (CMD_SETROTATE only adds the touch matrix, ADR 0011 amendment 1), from the build option
`O3OS_DISPLAY_ROTATE`, default **1**. ADR 0011 amendment 1 predicted 0 for the maintainer's unit; the first hardware
boot (OS-HW1, 2026-10-11) showed 0 upside down and 1 upright with this driver (`diag rot <0..7>` sets it live, not saved). A runtime setting and the boot-time flip chord are later (they need the settings store and the input).

**Size** (clean build, `-Os`, the linker's own figures): default image flash **47,648 B** (the HEX holds 47,704 B), SRAM_U **29,232 B**,
against the 4,100 B / 744 B of the OS-007b skeleton: +43.5 KB flash, +28.5 KB SRAM_U. Of the flash, by input: `app` 18.3 KB, `ui` 16.8 KB
(the four glyph faces and the icons are 12.9 KB of it), `core` 3.8 KB, **the display driver 5.6 KB** (chip 1.9, `hal::Display` 2.0, the
DSPI / eDMA / bring-up code 1.7), the rest the skeleton and the app's wiring. SRAM_U: the app 15.9 KB (`g_app_storage`), the display 12.4 KB
(the two staging buffers, 8,196 + 4,100 B, and its state). `-DO3OS_DEMO_CYCLE=ON`: 47,772 B; the fault-drill image: 47,568 B. The
1 MiB flash is 4.5 % used.

## Input (OS-007d, `input/`)

The device is interactive: the pattern, Song, Navigator and Learn screens answer the real controls through the OS-002a pipeline (HAL event,
gesture, binding, action). Written from the chips' data sheets. **Nothing here has run on a unit**: checklist INP below.

### The input hardware as found

| Control | Chip, bus | Count, layout | Source |
|---|---|---|---|
| **pads** | TCA8418 keypad scanner at 0x34 on **I2C2** (PTA12 SCL, PTA13 SDA, ALT5, the "grid" bus); INT = PTC11 | **48 = 12 x 4**; the matrix is 8 x 10, wired as a 10 x 4 block plus a 2 x 4 block (pad columns 10 and 11 on matrix rows 4..7, columns 9 and 8). **No velocity, no pressure** (`docs/design/product-ux.md` section 3): a press is velocity 127. Pad 0 is the top left, 11 the top right, 36 the bottom left (the highest notes on pads 0..11): checklist INP-4 | `docs/design/product-ux.md` section 3 |
| **buttons** | a second TCA8418 at 0x34 (the same address, the other bus) on **I2C0** (PTD8 SCL, PTD9 SDA, ALT2: the control bus, which **the SGTL5000 codec and the Si4703 radio share**); INT = PTD1 | **33** = codes 0..32 = `app::Button` (F1..F8 are matrix row 7, columns 7 down to 0; the other 25 a 5 x 5 block); the encoder has **no push** | `docs/design/product-ux.md` section 3 |
| **encoder** | the **FTM2 quadrature decoder** in hardware, PTA10 = phase A, PTA11 = phase B (ALT6, passive filter, pull-down: `PCR` `0x612`) | 96 counts a turn = **24 detents, 4 counts each** (96 pulses a turn, 24 detents); this tree reads the counter. No interrupt | NXP's device header |
| **pad LEDs** | IS31FL3731 at 0x74 on I2C2, a 16 x 9 constant-current matrix driver with an 8-bit PWM a LED | **48, one per pad, monochrome** (no RGB; the pads' LEDs are rows 0..3, columns 0..11 of the chip's matrix, four registers skipped a row). **The front-panel buttons have no LED**. Levels of the pad LEDs: a pad in use "front" 31 of 31 (gamma table to PWM 255), the rest "back" 15 of 31 (PWM 69) | the IS31FL3731 data sheet |
| handled by the platform (OS-POWER P1) | the power tact switch (PTC3) | not an OS-002a button (it is the unit's sleep / shutdown switch, and Shift + Power a screenshot): a platform input, `core::PowerHold` read once a pass ("Power switch (OS-POWER)" below); Shift + Power stays reserved for the screenshot | the power behaviour the official manual describes (a hold until a bar ends, **[public]**: documented, not verified; the times and the off state here are new) |

The first README of this tree put the SGTL5000's bus on "PTA12/PTA13": those are the pads' bus; the codec's is
PTD8/PTD9 (OS-005a found it). The TCA8418's INT is open drain, active low.

### Scanning: a main-loop pass, no timer, no interrupt

The spec asked for a scan at a fixed rate from a PIT or SysTick slot. The hardware has no GPIO matrix to sample: the TCA8418s scan, debounce
and queue (a ten-deep FIFO behind INT), the FTM2 counts the encoder. The one periodic job is **draining two FIFOs over I2C (about 115 us a register
at 375 kHz)**, and that must not run in an interrupt: I2C0 is shared with the codec (the two are serialised by being in the same context, with
no lock), and a bus wait at interrupt level would sit on the render and USB. So `Scanner::service()` (`scanner.h`) is **one pass of the main loop**,
which SysTick wakes every millisecond; the app's `poll()` is the consumer of a `core::InputQueue` (the SPSC ring, 64 events) whose producer is the
same loop. If the scan ever moves to a PIT (the pin-change row of ADR 0013's table), the ring is already the hand-over and nothing above it changes.
**No timer, no PIT, no NVIC priority, no DMA channel is claimed** (`resources.h` is unchanged; `mk66_dma_owner` passes; the PORT and I2C interrupt
lines stay disabled, so they stay at the unassigned priority).

A pass is bounded: per chip INT_STAT, a clear, the event counter and at most 4 events (a FIFO that holds more is finished on the next pass), every
transfer bounded in time (300 us a byte; the codec's register writes keep their 5 ms), about 0.6 ms a chip at the most, then one LED transfer at
most. A chip is read when its **INT level is low**, when more is known to wait, or by a **safety poll every 10 ms** (an INT that is not wired as assumed
costs 10 ms of latency, never a key). A chip that fails is left alone for a backoff of 10 ms doubling to 320 ms, its bus recovered (nine SCL pulses and a
STOP, open drain: only ever pulled low) from the second failure in a row, configured again before it is trusted; the other chip is not held up. Every 500 ms the
configuration is read back: a TCA8418 at its reset value lost its power without a bus error and would scan nothing for ever, so it is configured again and the
keys believed down are released.

### Debounce, publishing, the encoder

- **Lockout debounce** (`key_filter.h`, 5 ms): the first edge of a key is accepted at once (no added latency), then the key ignores the opposite edge for
  5 ms; a bounce is absorbed (the raw state is back at the accepted one), a release inside the lockout is delayed by the remainder, never lost. The chip
  debounces in hardware already: this is the second line. Edges that repeat the state are refused and counted (`stats().filtered`).
- **Publishing by comparison**: three states per key (raw, accepted, published). A queue that refuses an event loses nothing: the edge stays unpublished and is
  offered again next pass; `stats().dropped` counts the refusals ("drop newest and count"). A press and its release that both met a full queue are never seen,
  and the two states agree: **a lost release can never leave Shift stuck**. Events leave in non-decreasing time (a deferred event is stamped no earlier than the
  one before it) and **in the order the chip reported them, not in key order**: each raw edge takes the next number of a counter and the oldest unpublished
  accepted edge goes first, so Shift pressed before F8 in one drain reaches the gesture layer as Shift + F8 (redo), not as F8 (undo, F8 being the lower button).
  Within one pass the control buttons are read and published before the pads (two chips carry no time stamps: when a modifier and a pad arrive in the same
  millisecond, the modifier first is the reading that keeps Shift + pad).
- **Chip FIFO overflow** (more than ten events between reads). The CFG register is written with OVR_FLOW_M set, which (TI SCPS215G, Table 6 and Table 7: "overflow data shifts with
  last event pushing first event out") makes a full FIFO keep the **newest** ten and push the **oldest** out (with the bit clear, the default, the new event is the
  one lost). Once the kept events are in, the keys they name keep their state (the last event of each is the truth) and every **other** key believed down is released
  (`stats().resyncs`): its release may have been among the lost. The overflow flag is carried even if the drain that cleared it then fails, and a reconfiguration no
  longer clears it (the final flag-clearing step is left to the first drain). A failed **event read** may have popped its event with the byte lost on the way: it
  asks for the same resync. A transfer that fails elsewhere forgets nothing.
- **Encoder**: the hardware counts every edge of both phases, so nothing is missed however fast the dial spins and nothing is "illegal"; the scan reads the
  16-bit counter and `DetentAccumulator` turns the difference into whole detents with the remainder kept, net, **no reset on a reversal** (a bounce or the spring
  back into the detent is invisible). An event carries the detents since the last one
  (+ clockwise); a full queue keeps the turns and offers them again. **No acceleration**: an acceleration table over the counts of each 100 ms is a policy
  for the app, not the HAL (open question 8).

### One I2C driver for the codec and the input (the OS-005a driver, generalised)

`src/i2c.cpp` is OS-005a's polled master made **instance-generic**: one driver for I2C0 (the codec and the buttons) and I2C2 (the pads and their LEDs), the same
algorithm, divider (0x1F: 375 kHz) and pin handling. The seam is `I2cBus` (`i2c_bus.h`: `transfer(address, tx, tx_len, rx, rx_len)` with a repeated start,
`recover()`); the input drivers see only that (their tests run against modelled chips with injected failures), and `i2c::bus(instance)` is the one object per
peripheral. The codec's `i2c::init()`, `write_reg()` and `read_reg()` keep their signatures and wait 5 ms a byte as before; the input's `transfer()` waits 300 us.
`i2c_config.h` holds the instances' facts (base, clock gate, pins, mux) and the table of who talks on which bus. Rules for every client: **main thread only**,
polled (nothing enables an I2C interrupt, so nothing competes for the flags). Bringing the input up re-initialises I2C0 once more before the audio start does;
the second initialisation is harmless (a bus clear, the same pins and divider).

**The pad bus (I2C2), OS-BUG-PADS** (`input/pad_bus.h`; new Community OS behaviour, written after a report of dead pads on a plastic-back unit, **untested on that board
revision**): (1) the bus is not touched at the input bring-up. A gate opens it once the board's power key (PTA24, which feeds the SDRAM and possibly the pad boards: an open research question) has been on for
1 s (`PadBus::kSettleMs`, counted from `boot::rail_up_ms()`, which is 0: `early_pins` raises the key before SysTick starts, so the gate opens at an uptime of 1 s, a little more than a second after the rail) **and** the audio start is over (`input_audio_started()` in `main`); until then the pad scanner and the pad LED
driver are not asked and nothing is recorded; the control buttons and the boot chords are not delayed (I2C0). The first access initialises the module, which includes the bus
clear. (2) The grid bus runs with a **glitch filter** of 4 bus clocks (I2C2_FLT[FLT] = 4, about 44 ns; C2 = 0), written at the module's initialisation (`i2c_config.h`
`InstanceFacts::filter`); I2C0 is not changed. (3) A **NAK no longer re-initialises the module**: it is the bus working, so the pad bus re-initialises (and pulses the clock)
only after a timeout, an arbitration loss, a stuck bus, or when SCL or SDA reads low. (4) A pad scanner or LED driver that has **never answered** is asked again once a second for ever
(`scan::kPadRetryUs`, one bounded transaction), not at the growing backoff, which a chip that has answered before keeps. `Scanner::pads_gave_no_answer()` (never answered and 3
refused tries, `scan::kPadBootAttempts`) is what blink code 13 shows for the pads. The INPUT TEST screen therefore shows the pads offline from power-on until the gate has opened and the first configuration has gone through (the audio start included, so seconds, not milliseconds). **Warm reset:** the pad LED driver keeps its last frame lit until the gate opens, and a press on a pad before the gate is not scanned. This is expected. **Origin:** the values (FLT = 4, PCR `0x144`, the 1 s gate, the 2 ms wait) follow a bring-up reported to work on later boards. The behaviour is new Community OS behaviour, untested on the affected revision.

### Pad LEDs

`hal::Leds` (`hal/leds.h`): 48 monochrome LEDs, `set(index, level)`, `clear()`, `flush()`, `online()`. The app stages levels and the driver sends what changed,
**one bounded transfer per `flush()` call** (a row's span, 13 bytes at most, about 0.4 ms). `PadLeds` (`pad_leds.h`) is a state machine advanced by `flush()`:
the setup runs in an order that keeps the pads dark (the chip's registers outlive a reset of the MCU, so it may still show the last frame of the previous run):
**shutdown first**, a 10 ms pause **done by looking at the clock**, the function registers (picture mode, frame 1, audio sync off), then, still in shutdown, the on bits
zeroed, blink and all 144 PWMs cleared, only then every pad's on bit set, and **normal operation last** (that the registers take writes in software shutdown is
the datasheet's reading, INP-7), the frame diff, a **health read-back of the shutdown register every 500 ms** (a chip that has reset itself is detected) with the
whole setup again if it is not in normal operation, and the backoff and recovery of any chip. Brightness is perceptual (`level` 0..255, gamma 2, a nonzero
level is at least PWM 1) and never above PWM 255; **no total-current cap is applied**: the app decides what to light.

The default palette (`app/pad_palette.h`) is drawn from the theme's tokens: Front from the luminance of the Accent token spread over [160, 255], Back from
the Sel token's over [40, 132], whose PWM is at most 69 (back level 15 of 31). The result: Amiga front 216 (PWM 183) and back 71 (PWM 20), Black 220 (190) and 76 (23), White 196 (151) and 71 (20): a
touch under the full front level and a good deal under PWM 69, to be tuned on the unit (INP-7, open question 10). In the image, a pad that is down lights at the front level, and on the
INPUT TEST screen the others light at the back level, so the hardware session sees both. (An instrument-by-colour palette needs RGB pads.)

### The INPUT TEST screen

A hidden screen (`app/screens/input_test_screen.h`, one registry row with `hidden = true`: not in the Navigator, no screen button), a hardware test:
all 33 buttons (by name), all 48 pads (by index) and the encoder, live, from the raw events before the gesture layer. A control
that is down is the selection blue; one that has been pressed since the last clear shows its **press count** (a bounce reads as 2 for one touch of the
finger); the encoder shows its total, the last turn and the number of events; the top strip shows the key scanners answering (PADS OK, KEYS OK) and the
counters `ERR`, `DROP`, `FLT`, `RSYNC`; the bottom strip shows the age of the last event when the app took it (scan to app, the first part of the
press-to-screen latency), the longest, and the number of events. Its context **captures**: no control acts while it is up.

- **Boot chord**: hold **Shift + F1 + F8** while the unit powers up (read by a poll of at most 150 ms right after the app is built and the key scanners have been
  serviced, `boot_chord.h`; it ends at once when all three are down, so it also tests that a key held at power-up reaches the chip's FIFO, INP-8). The choice
  survives a display re-initialisation (the rebuilt app is sent to INPUT TEST again). The image built with `-DO3OS_INPUT_TEST=ON` starts on the screen with no chord.
  OS-DIAG1, why the first version (a fixed 40 ms scan) did not open on the unit: **not settled from the code**; the fixes are the poll, the survival of the choice, and
  a second log line (below) that tells the remaining cases apart.
- **Leave**: **F4 + F5** together (Pattern). **Clear the counts**: **F2 + F3** together. Neither is a subset of the boot chord, or the held chord would close it.
- LED code **13** at boot = a key scanner did not answer (it is retried; the screen shows `PADS NO` / `KEYS NO`).

### Tests

Host (`unit`, ASan/UBSan) and QEMU (`qemu_mk66`): `mk66_input_map_test.cpp` (the matrices: each pad and button reached once, the inverses, the corners),
`mk66_input_filter_test.cpp` (the lockout against bounce patterns, the refusals, a refused edge offered again, the tick wrap; the detent accumulator over every
transition, a fast spin, a reversal, the counter wrap; edges published in the chip's order, the order counter wrapping, `release_except`), `mk66_input_scanner_test.cpp` (the TCA8418 protocol in the order of its configuration and the order of a drain; the scan against modelled
chips: events and timestamps, bounce, an INT that is not wired, held-at-power-up keys, Shift before F8 in one drain, FIFO overflow in the newest-kept overflow mode (the oldest event pushed out; an overflow flag that survives a failed next transfer, a failed INT_STAT read, a failed event read), the full-queue policy and its convergence, the encoder through the queue, a dead chip with its backoff
and recovery, a chip that loses its power with no bus error, the tick wrap, and a 6,000-pass randomised run holding the invariants), `mk66_input_leds_test.cpp` (the IS31FL3731 driver against its model:
matrix position, the curve and its limit, the setup sequence one transfer a call, **the write order that keeps the pads dark (a chip still lit by an earlier run stays dark from the first shutdown write to the first frame)**, the frame diff, the health read-back and a chip that reset itself, a failing bus), `mk66_input_pins_test.cpp` (no
pin claimed twice across the board, every register constant against the fact tables). Also `input_test_test.cpp` (the screen's picture, the palette, the hidden screen driven by scripted input:
every control seen and none acting, the two chords; the scan and the app together: Shift then F8 from one drain is REDO). The gallery gains `inputtest_typical` and `inputtest_worst` in every theme (999 words of 1,792 in the dearest state; 1,182 in the VERTEX2F fallback).

### Image sizes (clean build, `-Os`; the baseline is the tree at the OS-005a / OS-008a merge)

Default image: flash **74,060 B** (the HEX holds 74,156 B), SRAM_U **38,736 B**, against 65,532 B / 36,568 B: **+8.5 KB flash, +2.2 KB SRAM_U**. By symbol: the input drivers about 4.5 KB
(scanner, TCA8418, LED driver, device), the INPUT TEST screen and the state 2.0 KB, the generalised I2C master 0.9 KB, the composition root 0.7 KB; RAM: the scanner 1.8 KB (the 64-event queue
520 B, two filters of about 0.8 KB with their order counters), the LED driver 0.1 KB, the app's view state about 0.2 KB. `-DO3OS_DEMO_CYCLE=ON`: 74,196 B (HEX 74,292); `-DO3OS_INPUT_TEST=ON`: 73,984 B; `-DO3OS_AUDIO_TEST=ON`: 74,104 B; the fault drill 73,984 B.
All variants build with 0 warnings, pass the static image checks and `ctest`, and the default image is reproducible (two clean builds, identical HEX SHA-256).

### Deviations from the spec text

1. **No PIT or SysTick-slot scan interrupt**: see "Scanning". The hardware has nothing to sample at a fixed rate, and the bus must not be used from an interrupt.
2. **No software quadrature state machine**: the FTM2 decoder counts in hardware, so "all transitions, illegal ones, fast spin" are tested as the counter's deltas through the detent accumulator
   (a skipped state is +-2 counts, a fast spin is many detents in one read, the wrap is in the tests).
3. **The LEDs are monochrome and the buttons have none**: `hal::Leds` carries a brightness, the palette is a brightness per role from the theme tokens.
4. **A shared I2C driver**: OS-005a's was made instance-generic rather than duplicated; its codec functions keep their signatures.
5. The palette lives in `app/pad_palette.h` (a small second file in `app`, beside the INPUT TEST screen), because it needs the theme and the platform may not include `ui`.
6. The hidden screen needed three small framework changes in `app`: `ScreenEntry::hidden` (and `navigator_screens()`, the Navigator's prefix of the registry), `Screen::captures()`, and the raw-event tap in `App::run_once`.

## SD card (OS-007e, `storage/`): read only, FatFs, a project loaded from it

The Tracker's SD card is mounted and read: its FAT32 (or FAT12/16, or exFAT) volume through FatFs, the card itself through the MK66's SDHC controller. **Everything that
reads goes through a contract that cannot write** (`hal/storage.h`), because the card holds the user's projects; since **OS-006c** exactly one other
client can write, the firmware writer (section "Firmware update over USB"), narrowly: `Firmware/` of a writable FAT card (since OS-FWANY, 2026-10-13, and OS-ANYCARD, 2026-10-17: any SD card, no marker file, no build option), nothing else (the layers below,
"As built in OS-006c"). At boot, after audio is up (and, since OS-USBAUDIO, **before** USB: the device settings are on the card and decide the USB mode), the card is initialised and mounted (`boot: sd mounted, FAT32, 14120 MB` in `diag log`), and if `Workspace/project.mt` exists the OG project in that folder is
loaded into the document, in slices of the main loop. `diag sd`, `diag ls <path>` and `diag load [folder]` show and drive it. **Nothing of this has run on a unit**: the
SDHC register policy is the one part QEMU and the host cannot run (checklist SD-* below).

### How read-only is guaranteed

1. **`hal/storage.h` has no write call.** The contract the layers above use (`mount`, `exists`, `open`, `read`, `seek`, `size`, `close`, `opendir`, `readdir`, `closedir`) has no
   create, write, mkdir, remove, rename or sync, so nothing above can ask for one and no implementation can be handed one. The host backend (`platform/host/dir_storage.h`) only ever
   calls `open(O_RDONLY)`; `tests/og_folder_test.cpp` snapshots a directory tree before and after a whole project load and requires it identical.
2. **FatFs is configured read-only** (`third_party/fatfs/ffconf.h`: `FF_FS_READONLY 1`): `f_write`, `f_sync`, `f_mkdir`, `f_unlink`, `f_rename`, `f_truncate`... are not compiled. The glue
   (`storage/src/fat_storage.cpp`) defines `disk_status`, `disk_initialize` and `disk_read` and **no `disk_write`** (and no `disk_ioctl`): ff.c in this configuration references only
   those three (`nm` of the object shows it), so a build that began to need a write would not link.
3. **The block device has no write call** (`storage/block_device.h`: `present`, `start`, `ready`, `block_count`, `read`).
4. **No write command exists in the SD driver.** `sd::Cmd` (`storage/sd_protocol.h`) is an allowlist of 15 commands: CMD0, 2, 3, 6, 7, 8, 9, 12, 13, 16, 17, 18, 55 and ACMD6, 41. There is no
   WRITE_BLOCK (24), WRITE_MULTIPLE_BLOCK (25), PROGRAM_CSD, SET/CLR_WRITE_PROT, ERASE_WR_BLK_START/END, ERASE, LOCK_UNLOCK or GEN_CMD (56) anywhere in the tree: not as a constant, not in a
   table. `SdBus::command` and `read_data` take a `Cmd` and `SdhcBus` refuses a value that is not `known()` before it writes a register; `mk66_storage_protocol_test` walks all 256 values of
   the type and requires exactly the 15, and `mk66_storage_card_test` runs a card's whole life (init, reads, errors, a second init) and checks every command it was sent. The data phase
   the SDHC code implements is a read (DTDSEL 1; there is no code path that fills the FIFO).

Write-protect: a card with its write-protect tab set works the same for reading; the tab is also a hardware backstop the maintainer can use for the first sessions
(OS-007e's four layers below are as shipped then; OS-006c changed layers 2 to 4, and the next block says how).

**As built in OS-006c** (the firmware writer; `hal/storage.h`, layer 1, is unchanged and still has no write call):

2. FatFs is configured `FF_FS_READONLY 0` (`ffconf.h`), so ff.c compiles its write functions (flash +6.1 KB, `.bss` +0). `FatStorage` opens files `FA_READ` only and calls none of
   them. **Only `update/src/fat_firmware_store.cpp` may name a write-class FatFs symbol** (`f_write f_sync f_unlink f_rename f_truncate f_mkdir f_expand f_setlabel f_mkfs f_chmod
   f_utime f_forward`) and the glue's `disk_write` / `disk_ioctl` are defined only in `storage/src/fat_storage.cpp`: `tools/check_layers.py` rule 6 fails the build otherwise (ctest
   `layers`, with self-test cases). Before each write-class call the store applies the path guard to the full path.
3. `BlockDevice` has `write` and `write_protected` since OS-006c; the glue's `disk_write` is the only caller. `Card::write_blocks` refuses (sending nothing) a card whose CSD says
   permanently or temporarily write protected or whose slot switch (`SdBus::write_protect_switch`, not known to be wired on the Tracker: always false) says protected.
4. `sd::Cmd` is an allowlist of **16** commands: the 15 above and **CMD24 WRITE_BLOCK**. CMD25, PROGRAM_CSD, SET/CLR_WRITE_PROT, ERASE_WR_BLK_START/END, ERASE, LOCK_UNLOCK and
   GEN_CMD are not there, as constants or otherwise (`mk66_storage_protocol_test` walks all 256 values: exactly 16 accepted, CMD24 the only one with a write data phase).
   `SdhcBus::write_data` is the SDHC write phase (below, "Firmware update over USB"); `command()` and `read_data()` refuse CMD24.

### The hardware, and where each fact comes from

| Fact | Value | Source |
|---|---|---|
| Lines | PTE0 D1, PTE1 D0, PTE2 CLK, PTE3 CMD, PTE4 D3, PTE5 D2, all ALT4, high drive; CMD and D0-D3 with pull-ups | RM PORT; the SDHC pin-mux table |
| Card detect | PTD0 (Teensy pin 2), GPIO input with a pull-up, LOW = a card is in | the Teensy 3.6 pin map |
| Clock | `SIM_SCGC3[SDHC]` gate, `SIM_SOPT2[SDHCSRC]` 0 = the core clock, 180 MHz | NXP's device header |
| Speeds | 400 kHz identification (375 kHz made: SDCLKFS 0x10, DVS 14), 25 MHz (22.5), then CMD6 high speed 50 MHz (45 MHz made: SDCLKFS 0, DVS 3) | the SD Physical Layer spec; `plan_clock` computes the dividers (test) |
| Bus | 4 data lines (ACMD6) | the SD Physical Layer spec |
| Power | none switched: the slot has no power switch (the NXP SDK's host port waits 1,000 ms "to make the card stable"; this driver waits 2 ms + 80 clocks, the card has been powered for the whole boot) | RM; the NXP SDK |
| Registers, bit positions | SDHC at 0x400B1000; XFERTYP, PRSSTAT, IRQSTAT, SYSCTL, PROCTL, WML... | RM "SDHC", MK66F18.h; every bit pinned by `mk66_storage_soc_test` |
| Interrupt | IRQ 81 `SDHC`, row Completion (80) in `irq_map.h`; **not enabled** | see below |

The reference manual itself was not machine-readable for this slice (as for the USB driver): the places where a value comes from memory of the RM and not from the header fact tables are marked "(RM)" in
`sdhc_facts.h` and are on the checklist: the DAT0 busy bit (DLSL bit 0) and the R2 response register layout (a CID that decodes to the label of the card proves it).

### Polled, not ADMA2: why

The driver reads the controller's FIFO from the main loop: wait for the watermark flag (`PRSSTAT[BREN]`, 16 words), read 16 words from `DATPORT`, repeat; a block is 8 groups. Why not ADMA2: the transfers are 512 B to 16 KB
made by the main loop, which waits for them anyway; the controller delivers a block in about 23 us on four lines at 22 MHz and the loop reads it out in under 7 us (the FIFO holds 128 words), so DMA would
save no wall-clock time and, at 45 MHz, only a fraction of the loop's cycles; it would cost a descriptor table, a 4-byte alignment rule on every caller's buffer (FatFs reads straight into the caller's), the DMA
error and boundary recovery, a SYSMPU master grant and an enabled interrupt. So **no eDMA channel, no PIT, no interrupt** is claimed (`resources.h` says so; the IRQ stays in its Completion row for the day an
ADMA2 version raises a flag there) and the audio path cannot be delayed by the card beyond the main loop's own preemption. A later slice that streams samples into SDRAM while audio plays can put ADMA2 behind the
same `SdBus`; `diag sd`'s counters and the measured read speed (SD-7) are what decides.

Every wait is bounded (`sd_policy.h`): a command's answer 10 ms, a busy card 500 ms (100 ms for the stop after a failed read), a data phase 250 ms + 2 ms a block, ACMD41 the spec's second, the controller's own resets 100 ms. A
card that does not answer costs at most `worst_init_us()` (< 3 s, tested) at the mount and `worst_read_call_us()` (< 0.5 s, tested) for a read; each failure resets the controller's circuits (RSTC, RSTD) and
stops the card (CMD12), so the next call finds the bus idle. A read is chunked at 32 blocks (16 KB, about 0.7 ms): a long read is a series of short blocking calls. A CRC error is retried twice, a timeout never.
Everything runs in the main loop; nothing is called from an interrupt.

### FatFs: configuration, cost, version

`third_party/fatfs` (R0.15a; `THIRD_PARTY.md` has the SHA-256 of the archive, the licence and the five-file list). Configuration: read-only, long file names with the **static** working buffer (no heap, no stack),
UTF-8 at the API, CP437 for 8.3 names, **exFAT on** (+1,480 B of code, +608 B of `.bss`; the card is FAT32 and the OG needs FAT32, but SDXC cards are exFAT as sold, so a card the user did not reformat
mounts and lists), one volume, 512-byte sectors, no re-entrancy, no file lock. Cost of the whole slice, measured on the clean `ci.sh mk66` image (default build, `-Os`) against the previous integration build: **flash +30,036 B** (164,564 to 194,600; of which FatFs 6.8 KB with exFAT; the OG readers it reaches
11.0 KB, which the app did not link before; the loader 2.3 KB; the SD protocol, SDHC driver, storage glue and diag text 7.8 KB; console, main and app_main 2.0 KB), **`.bss` +11,992 B** (61,680 to 73,672: the loader and its
three readers 8.3 KB, FatFs and the volume 3.4 KB, the card 0.2 KB; SRAM_U in use 76,744 B of 192 KB), no SRAM_L, no SDRAM, no eDMA channel, no PIT. **Version:** R0.15b is not used because of a `f_readdir` end-of-directory bug upstream fixed in R0.16; see THIRD_PARTY.md. A found-by-ASan fix:
FatFs keeps a pointer to the volume object it was mounted with; `FatStorage`'s destructor takes it out (the image's one is static and never runs it, the tests' are on stacks).

### What is loaded at boot, and from where

An OG card holds its **Workspace** (`Workspace/project.mt`, `Workspace/patterns/pattern_NN.mtp`,
`Workspace/instruments/instrument_NN.mti`; the card's `Projects/<name>/` folders hold the saved projects). The image loads the Workspace at boot: if `Workspace/project.mt` exists, `Workspace` is loaded; if not, `boot: sd has no Workspace/project.mt: nothing to load` and the document
stays the fresh one. `diag load Projects/<name>` loads any folder (`load` alone is `Workspace`), which is how a card with no Workspace is tried. The folder layout and the readers are `docs/formats/og.md`; the loader
(`app/og_folder.h`) reads the project file (`og::ProjectReader`), every `patterns/pattern_NN.mtp` the directory lists (`og::PatternReader`), every `instruments/instrument_NN.mti`, `NN name.pti` and
`instrument_NN.pti` (`og::InstrumentReader`; a `.pti` is read for its header, a `.pti` beside an `.mti` of the same slot wins) and counts the rest as ignored (`patternsMetadata`, `._` files, subfolders).
**Not loaded by this reader:** the sample PCM (a `.pti`'s frames, the WAVs an `.mti` points at), which `app/og_samples.h` loads into the SDRAM sample arena right after it (OS-007f, below);
`patterns/patternsMetadata` (the pattern names), which is not in this slice's scope. A file that fails (a bad CRC, a truncated file) is counted and the load goes on; a card that fails ends it
(`storage_error`).

The load is a slice of the main loop (`appimg::pass()`: 16 KB or about 2 ms a pass, so a typical project is a few dozen passes). While it runs the app's frames are held: the screen keeps its last picture (the display is still
serviced), because the project is being replaced under the screens. When it ends the undo history is reset (the project changed behind the editor's back; the editor would notice, this is explicit), the app is
re-attached to the document (its cursor is clamped to what exists; a pattern that does not exist shows empty) and the result is logged:

```
boot: sd mounted, FAT32, 14120 MB          (the numbers here and below are made up, to show the shape)
boot: loading Workspace
boot: project loaded
Workspace: v16, 12 patterns, 20 instruments
load: bytes read 211884
boot: samples 20 loaded, 0 missing, 2832 KB
```

**The samples (OS-007f).** Once the project is in, its sample instruments get their audio (`app/og_samples.h`): `.pti` slots from the `.pti` (the PCM after its 392 bytes), the others from `samples/instrNN.wav`; each is bound to the voices (window,
loop, volume, tune, fine tune). The transport is stopped when a load starts and the voices are let to die away first; the voices' table is cleared (the demo kit goes), the PCM is read in the same 16 KB slices with the frames held, and
`boot: samples N loaded, M missing, K KB` ends it (also on a `pcm:` line of `diag sd`). A missing or unusable file leaves its instrument silent, one line each in the log (8 at most); an instrument that does not fit the rest of the
arena (10,816 KiB) is silent too, `load: sample arena full, instruments silent N`. A project with no sample instrument gets the demo kit again. Hardware rows SD-13 to SD-15.

### A card taken out and put back (OS-SDSWAP, `storage/card_swap.h`)

Until OS-SDSWAP the card was mounted once, at boot: a swap left FatFs and the card driver holding the first card, and every read of the second one failed with an i/o error until a reset.
Now the main loop follows the slot (`appimg::pass` calls `storage::device::poll(ms)` first thing; no interrupt, no timer):

- **The pin** (`PinDebounce`): the card-detect level has to hold for 250 ms before it counts, so a card sliding through the slot is one Removed or one Inserted, never a burst.
- **Removed**: the volume is released at once (`FatStorage::release()`: every handle closed, FatFs forgets the volume, the driver forgets the medium), so every card call answers
  `NotMounted` (the `ls` command says "the card is not mounted", the browsers "NO SD CARD") instead of failing on a bus with nothing on it, and the strip says `CARD REMOVED`. (From the
  moment the pin opens `mounted()` is already false; the 250 ms only decide when the volume is let go.)
- **Inserted**: the strip says `READING CARD`; two frames later (so that it has been drawn) the card is initialised and mounted (`device::mount()`, which blocks like the boot mount: tens of
  ms for a card that answers, at most about 3 s for one that does not, the display serviced from the idle hook), then `CARD READY` or `CARD NOT READABLE`, and the open File / Sample load
  listing is read again (`App::card_changed`). A card that does not mount is not tried again until it has been taken out and put back (`sd mount` from the console still retries). The mount waits
  while a save, a project load or the firmware update path has the card; a pull ends those within a pass or two. The project in the document is never touched.
- **Writers**: a save (`FatStorageWriter`) pins the mount it began on (`FatStorage::generation()`, with the volume serial); a firmware upload (`FatFirmwareStore`) does the same. Any release
  or remount, the SAME card put back included, ends them with `NoCard` ("SAVE ... FAILED: NO CARD", "ERR no card" on `fw put`); nothing is retried onto the card that comes back, and
  the abort of an upload does not delete anything on it. The audio path never touches the card (its samples are in SDRAM, the modules in RAM): the render does not notice a swap.
- **Who has the card** (`CardOwner`: Tracker, NoCard, Host): `CardSwap::hand_to_host()` (refused while `card_busy()`: the claims of `card_claims.h`) and `take_back()` are the whole hand-over for the USB disk mode (OS-USBDISK); while the
  owner is Host `poll()` reports nothing, `release()` is a no-op, and `reacquire()`/`mount()`/`sd mount`/`fw` refuse (`ERR card on host`). The MSC engine and its bridge are in `usb/` and `usb/src/msc_bridge.cpp`. `sd` prints `owner:   tracker, N removed, M inserted since boot`.
- **Boot is unchanged**: the boot mount and its logging are as they were (a card that is in and does not mount is still the boot fault); `device::watch_begin()` only starts following the slot after it.

Tests: `tests/mk66_storage_swap_test.cpp` (debounce, release, remount, stale handles, a card that does not mount, Host, a save and a firmware upload across a swap; host and QEMU) and
`tests/file_screen_test.cpp` (the strip and the rescan). Unit: `tools/hil/qa_sdswap.json` and `qa_sdswap_save.json`.

### The `diag` commands of this slice

| Command | What it does |
|---|---|
| `sd` | slot (detect switch), mount (kind, MB, cluster), card (SDHC/SDXC/SDSC, spec, CSD version, MB and blocks), bus (lines, kHz, high speed), identity (manufacturer, OEM, product, revision, serial, date), RCA and OCR, the raw CID and CSD, counters (inits, commands, reads, blocks, retries, failures) and error counts, the last project load, the write counters, and `access: read only except the firmware writer` |
| `sd mount` | runs the card's initialisation and the mount again (a card put back in, a retry); blocks for the card's own time with the display serviced; not while a project loads |
| `ls [path]` | the directory (root by default), a few lines a pass, a directory as `d  name/`, a file with its size; any key stops it; spaces in the path as typed |
| `load [folder]` | starts loading an OG project folder (default `Workspace`) into the document; `sd` shows the result. Replaces the project in memory; reads the card, writes nothing |
| `inst <1..64>` | OS-007h: what the voices were given for instrument n: play mode, window start and length (frames), loop, root, rate, volume (of 127, and as a gain), slice count or window size, and the 16-bit sum of the first 64 samples of the window. A memory read |

### Layout of the code

`platform/mk66/storage/`: `sd_protocol` (commands, argument and response encodings, CSD and CID, the CMD6 status), `sd_policy` (every timeout, the chunk size, the retries), `sdhc_facts` (registers, bits, pins, the clock
divider search, the transfer-type word of each command), `sd_bus` (the seam between the protocol and the controller), `sd_card` (the identification sequence and the block reads: `Card` is a `BlockDevice`), `fat_storage` (FatFs behind
`hal::storage::Storage`; the `disk_*` glue), `sd_text` (the text of `diag sd` and one `ls` line), `ls_command` (the pager); in the image only: `sdhc_bus.cpp` (the register policy) and `device.cpp` (the three static
objects). `app/og_folder` is the loader (it is the only code that knows the OG folder layout), `platform/host/dir_storage` the host backend (a directory as the card, case-insensitive like FAT).

### Tests

On the host (ASan, UBSan) and QEMU's Cortex-M4F: `mk66_storage_soc_test` (every register bit against the NXP header, the pins, the clock divider search against the known dividers and exhaustively, the transfer-type word of
every command by hand, the timeout bounds), `mk66_storage_protocol_test` (the allowlist over all 256 values, argument and response decoding, the CSD of a known 8 GB card and of 2 GB, 64 MB, 2 TiB-class cards, the CID, the
CMD6 status, the R2 layout), `mk66_storage_card_test` (a software card: the init sequence in order for SDHC, SDSC and version 1 cards, the high-speed step and its failure, no card, a card that never gets ready (bounded at the spec's second),
a wrong CMD8 echo, an SDUC card, every read path, retries, timeouts, a card pulled out, and that no write-class command is ever sent), `mk66_storage_fat_test` (a FAT32 image built in the test from byte maps with long names, one-sector
clusters and a directory spanning clusters, and an exFAT image: listing, reading in chunk sizes 1 to 20,000, seek and end of file, case-insensitive names, every error answer, the handle pools, a medium absent, blank, unreadable or taken out,
the whole stack on a software card, `diag sd` and `ls` through the real shell). Host only: `og_folder_test` (the host directory as the card against the same contract, one OG project folder loaded in slices and compared with the model the
readers make from the same bytes, the card unchanged afterwards, a missing or bad project file, abort and restart, a card that fails mid-load) and `og_folder_fat_test` (the same folder on the FAT32 image and through the card stack).

### Deviations from the task text, and what is not here

- (OS-007e as shipped; OS-006c adds the firmware writer, below) No write path, no `mkdir`/`remove`, no `SD.stop()`-style unmount (the card is mounted once; `sd mount` mounts again), no hot-plug event: a card pulled out makes the next call say `NotMounted`; the user puts it back and runs `sd mount`.
- The `Storage` contract is a small abstract class (like `hal::Display`), not an object-per-file API: handles name slots of a fixed pool (2 files, 2 directories), so nothing allocates and nothing is owned by the caller.
- The simulator (`o3sim`) has a card since OS-E2E-1b: the scenario command `card <fixture-dir>` copies a fixture folder to a scratch directory and mounts it through `DirCard` (`platform/host/dir_card.h`: `DirStorage` for the reads, a write side behind the real path guard); a `--card <dir>` option is not needed.
- The card detect is read without debounce at the mount (the card sits still at boot); a later hot-plug item adds it.
- LED: no blink code for an SD failure (a missing card is normal); `diag stage` lists "sd card (not mounted)" when a card is in the slot and did not mount.

## USB and the `diag` console (OS-006b, `usb/`)

The MK66's USB0 (full speed, device mode) enumerates in **STANDARD mode: USB-MIDI and a CDC serial port** (or, since OS-USBAUDIO and by default, in AUDIO mode, which has both too: the device setting USB AUDIO decides), and the serial port runs the `diag`
console (ADR 0011 item 1). Identity (maintainer decision, ADR 0012 amendment 3): **16D0:0E9F**, device name **"Track3r"**, `bcdDevice` 0x0001 in STANDARD
(AUDIO mode, not exposed by this driver, is 0x0101: the same VID/PID with another REV so that a host re-reads the descriptors per mode);
serial number = 16 hex digits folded from the chip's 128-bit unique ID (ADR 0011 item 6). AUDIO mode's descriptors exist and are tested; this driver never
presents them (its isochronous endpoints are a later slice). Until OS-006c nothing here wrote `_fwinfo`, drove pin 81 or updated anything: `reset` is a SYSRESETREQ, and `fw` (section "Firmware update over USB") is the one command that does the rest.

**Where the code is.** The logic is the pure `usb` library, tested on the host (ASan/UBSan) and on QEMU against a fake controller
(`tests/usb_fake_controller.h`, `usb_rig.h`): `control.h` (endpoint 0: every standard request, the CDC and UAC2 class requests, ZLP and toggle rules,
stalls), `endpoint.h` (the buffer-descriptor word, the ping-pong and DATA-toggle bookkeeping, the endpoint table of a mode), `device.h` (the whole
interrupt: bus reset, tokens, SOF, suspend; a template over a register policy), `cdc.h` (line coding, the byte rings, USB-MIDI event packets),
`diag_shell.h` (line editor, tokenizer, dispatcher keyed by action id `diag.<name>`), `diag_log.h` (the log ring). What is in `platform/mk66/usb/`:
`soc_facts.h` (register addresses and values, the clock proof, the UID serial; host-tested), `diag_text.*` (the text of `usb`, `stage`, `crash`, `log`,
`input`, `version`; host-tested goldens), and the image-only `src/usb0.cpp` (the register policy, the objects, the interrupt handler, the bring-up) and
`src/console.cpp` (the command table). **Not covered by any test: the register writes themselves** (QEMU has no USB0): they are the hardware checklist below.

### The clock: IRC48M, because nothing else can make 48 MHz at 180 MHz

The full-speed module needs 48 MHz within 0.25 %. The PLL path (`SIM_SOPT2[USBSRC]` = 1 through the `SIM_CLKDIV2` divider, out = in x (USBFRAC+1) / (USBDIV+1)) cannot
make it from the OS's 180 MHz in HSRUN: `soc_facts.h` proves by exhaustion that no divider setting is within 0.25 % (it would need a 192, 168, 144 or 120 MHz tree). So the source is the
**48 MHz internal oscillator IRC48M** (`PLLFLLSEL` = 3, `CLKDIV2` = 0), disciplined by the module's own clock recovery from the bus' SOF packets
(`CLK_RECOVER_IRC_EN` = IRC_EN|REG_EN, `CLK_RECOVER_CTRL` = CLOCK_RECOVER_EN|RESTART_IFRTRIM_EN). It does not depend on the system clock at all, so **HSRUN, the 90 MHz bus clock and
the SDRAM clock do not matter to it**; the module's registers run from the bus clock, 90 MHz. `SIM_SOPT2[PLLFLLSEL]` is shared with the other peripherals that take "the FLL or PLL clock" (`SDHCSRC`, `LPUARTSRC`, `TPMSRC` and `TIMESRC` of `SIM_SOPT2`; the UARTs 0 and 1 run from the system clock, UART2-5, MIDI DIN (UART4) included, from the bus clock, so the MIDI baud divider is computed from F_BUS): a later
slice that gives one of them that source must know the OS runs it from the IRC48M. The write is a read-modify-write, so CLKOUTSEL and FBSL (the SDRAM's) are kept.

### Bring-up and the pull-up (`usb0::start()`, in this order)

| # | Step | Source |
|---|---|---|
| 0 | the descriptor set of this first enumeration (OS-USBAUDIO: `start(mode, profile, usb_return)`, decided by `appimg::start_usb` from the device settings: AUDIO when USB AUDIO is On, else STANDARD; rebuilt here, before anything is touched, or STANDARD with a log line if the profile cannot be fed or the set does not build), then preconditions, nothing touched: the BDT is 512-byte aligned (declared by type, also checked at run time), the descriptors built and `wTotalLength` is their length | ADR 0011 lesson 1 |
| 1 | `SIM_SOPT2`: USBSRC set and PLLFLLSEL = 3 (read-modify-write); `SIM_CLKDIV2` = 0; `SIM_SCGC4`: USBOTG gate set | RM SIM |
| 2 | `SYSMPU_RGDAAC0`: bits 24 and 25 set (master 4, the USB module, read and write of region 0) | RM SYSMPU |
| 3 | `CLK_RECOVER_IRC_EN` = 0x03, `CLK_RECOVER_CTRL` = 0xA0, 100 us | RM USBFS (the wait is margin) |
| 4 | `BDTPAGE1..3` = the table, `ISTAT`/`ERRSTAT`/`OTGISTAT` cleared, `CTL` = USBENSOFEN, `USBCTRL` = 0 | RM USBFS flowchart |
| 5 | `UsbDevice::prepare()`: the table cleared, endpoint 0 armed to receive (both descriptors), `ADDR` = 0, `INTEN`/`ERREN` set, the state a bus reset leaves | usb/device.h `reset_bus()` |
| 6 | NVIC: pending cleared, IRQ 53 enabled (its priority, 64, was written at boot from the table in `irq_map.h`) | ADR 0013 D1 |
| 7 | `CONTROL` = DPPULLUPNONOTG: **the D+ pull-up, last** | RM USBFS |

**Pull-up timing:** the host cannot see the device until step 7, and by then the table, endpoint 0, the interrupt and the clock are all ready; the first thing that happens
afterwards is the host's bus reset, which `reset_bus()` handles exactly as `prepare()` did. A failed precondition leaves the pull-up OFF and says why in `diag log`.

### VBUS and suspend

The board's VBUS routing is not known (`docs/hw/tracker-hardware-notes.md` records no USB routing; ADR 0012 validation item 1 is the trace), so this driver does not read VBUS: it
switches the pull-up on at boot and leaves it. **No VBUS sensing** therefore, and these consequences, all on the checklist: a host that is
switched off and supplies no VBUS sees nothing; "unplugged" is seen only as the bus going idle (the suspend interrupt, 3 ms) and "plugged in again" as a bus reset, and both are
handled the same whatever the cause; there is no back-power protection beyond the board's. **Suspend does not power the transceiver down** (`USBCTRL` stays 0, no stop mode): the
unit has no 2.5 mA suspend budget to keep (the whole board stays powered), so suspend and resume are tracked (flags, counters, `diag log`) and nothing more. A resume is taken from the
resume flag or, if that is missed, from frames flowing again.

### The interrupt: priority, cost, bound, shared state

`USB0_IRQHandler` runs `usb::UsbDevice::isr()` at priority **64** (ADR 0013 D1; `soc_facts.h` asserts the row): the audio DMA slot-boundary interrupt (16), SysTick (32) and the MIDI UART (48)
preempt it, so the audio deadline and the MIDI timestamps never wait for USB; the SDHC / SPI2-DMA (the display) / I2C completions (80) cannot preempt it, but they are flags only and the transfers
they report are done by DMA hardware, so a completion flag delayed by one USB interrupt delays only the next display or SD chunk; the render (128) is preempted, as D1 requires.
The code runs from flash (it is not in `.ramfunc_l`).

- **Bounded:** at most `kMaxIsrLoops` = 16 passes per entry; a pass handles one finished token (a copy of at most 64 bytes and a few descriptor writes) or the pending flags. The STAT FIFO is 4 deep, so
  a normal entry is 1 to 6 passes; if more were pending the line stays asserted and the interrupt is entered again at once. A test drives a controller whose flag never clears and requires the cap.
- **Cost [est, to be measured]:** an SOF entry is about 100-150 cycles (under 1 us), a bulk token with a 64-byte copy 250-400 cycles (about 2 us), a control SETUP about 300-600 cycles; the worst
  realistic entry (4 tokens and the flags) is about 3,000 cycles (17 us), against the audio block of 261,120 cycles (1.45 ms, the DMA slot period): about 1 %. `diag usb` prints the **measured** last and worst
  entry in cycles (DWT), which is the figure ADR 0012's validation item 5 asks for.
- **No work that can wait:** no formatting, no parsing, no allocation, no SD, no loop whose bound is not a constant. Command parsing and execution are in the main loop (`console::service()`).
- **Shared state:** the buffer descriptors, the ping-pong pairs, the control pipe and every packet buffer belong to the interrupt alone. The four rings are the only shared objects and all are
  `usb::OverwriteRing`s (lock-free, single producer and single consumer, the producer never waits, an overrun drops the OLDEST and is counted): CDC transmit (main loop writes, interrupt reads), CDC
  receive (interrupt writes, main loop reads), MIDI to host (engine writes, interrupt reads), MIDI from host (interrupt writes, engine reads). The few flags the main loop reads (`state()`, the CDC control
  lines) are relaxed atomics; the counters have one writer each and are read as plain words, as in `usb/counters.h`.
- **Endpoint 0** sends one packet at a time (the next on the acknowledgement of the last), which costs nothing a host can see; an OUT data stage is limited to one packet (SET_LINE_CODING's 7 bytes is the only one).
  A stall is the one-shot `ENDPT0[EPSTALL]`, cleared by the STALL interrupt as it always was, plus a BD-level stall that lasts until the next SETUP (USB 2.0 9.2.7; OS-USBDISK int.14): both transmit descriptors of endpoint 0 are left owned with the STALL bit, which the controller answers without consuming them, so a host that retries the data-stage IN (or whose handshake was lost) is stalled again instead of NAKed until its timeout. The next SETUP or a bus reset zeroes them (`stall-cleared` in `usb ep0`); the receive descriptors stay armed for it, and nothing depends on a SETUP being taken while EPSTALL is set. The STALL interrupt writes CTL only for the first handshake of a stall, to end a token freeze a stalled data stage may leave (OS-USBDISK int.13); the later ones are counted (`stall retries` in `usb ep0`). An OUT retried after the one-shot is acknowledged and dropped. A bulk endpoint's halt is separate and stays until CLEAR_FEATURE; one microsecond-wide gap remains where a bulk halt's handshake can be taken for the stall's first (`stall_ep0` drops a stale flag first).
  The K66 has **one EPSTALL per endpoint number**, so halting a bulk IN halts the OUT of the same number; GET_STATUS reports it so.
- **CDC transmit never blocks:** a host that does not read loses the oldest chunks, counted (`usb` shows `dropped-chunks`) - but replies are paced by the room check below, so this happens only to output nobody asked for (the log of a dying run, a stream with the terminal closed). Every packet is at most 63 bytes, so none is full and no zero-length packet is ever needed.

### Back-pressure: a paste loses nothing (review fix)

The first version could lose the heads of replies: two or three long commands pasted at once start more output than the 3.9 KB transmit ring holds, because the main loop writes a whole reply in
microseconds and the host takes a packet a millisecond. Every link of the chain now says "not yet" instead of dropping: **(1)** the shell takes input only while the transmit ring has room for
a reply (`kConsoleFeedRoomBytes` = 1.6 KB free, the largest a command makes; `Shell::feed` returns how many bytes it took and `usb/diag_pump.h` keeps the rest for the next pass), and not while a pager owns
the terminal; **(2)** the receive ring then fills, and from `kCdcRxHoldChunks` = 12 of its 15 chunks the interrupt stops re-arming the bulk OUT descriptors, so the host is NAKed and retries the packet
itself (the next start of frame re-arms them once the main loop has read the ring down; `usb` counts the holds); **(3)** `log` is a pager (a non-interruptible stream) that writes a line only while
the output has room for it, so `log 48` x3 pasted in one burst returns every line of every reply. The price is that a paste is served at the speed the host reads. Tested end to end against the fake controller
(`tests/mk66_usb_console_test.cpp`: the real shell, pump, pager and device; hosts reading 1 to 19 packets a millisecond, a host that stops reading for 300 ms, a flood of 2,000 empty lines that
must be NAKed) and at the device level (`usb_device_test.cpp`).

### Start-up faults do not stop the image (review fix)

An audio failure used to loop in `boot::fail()` before USB came up, so `diag` was dead on the likeliest first-boot failure (the codec's I2C link or the MCLK divider). Now `main` latches the code (the LED shows 10 for
good, like the display's codes), logs `boot: audio FAILED, outputs muted, result N`, (a ClockFault adds `boot: audio clock fault at mcr|mdr|boundaries` and `boot: audio MCR 0x... MDR 0x...`, OS-HW1; `boot: display dma ok` or `FAILED, polled path` follows the display line), records the fault for `diag stage` and carries on: USB and `diag` come up, the audio status polling is skipped (no block clock to watch),
and the outputs stay muted (`start()` never unmutes). The display and app failures are listed by `diag stage` as well.

### Memory (measured by the build)

The image grows by **about 21.1 KB of flash and 10.0 KB of SRAM_U** against the OS-005a/007d baseline (the image 74,156 to about 95,250 bytes; SRAM_U in use 38,736 to about 48,690, of which `.bss` 36,080 to about 45,620; the build identity string makes the last few bytes vary from commit to commit). The review fixes (back-pressure, start-up faults, a table-driven `usb` text) cost about 0 net flash: the 64-bit division helper (about 800 bytes, pulled in by `Text::percent`, the `usb` text and the governor budget in `audio::Pipeline::init`, all now 32-bit) paid for them. Of the flash, the USB stack proper is
about 9.4 KB (control pipe 2.4, the interrupt and the register wiring 4.0, the descriptor builder 2.3, the rings 0.7) and the console about 9 KB (shell and log 1.8, the commands 2.8, their text 3.0, the audio
formatters 1.4 which were not linked before, with the 64-bit division they pull in). ADR 0012's 4-6 KB estimated only the audio part of the stack (UAC2 descriptors and requests, packetiser, feedback); the
EP0/BDT/ISR driver and the console were outside it. The structural validator is not in the image (2.3 KB; it is run by the host tests on the very descriptors the driver builds). RAM: the CDC transmit ring is 4.3 KB (it must hold the
longest reply, `usb`, 1.2 KB, with room: the main loop writes a reply in microseconds, the host takes a packet per millisecond), the receive ring 1.1 KB, the two MIDI rings 1 KB, the device 0.85 KB (packet buffers 640 B), the table 0.5 KB.
It comes out of the "rings" row of the SRAM_U budget (`mem_map.h`), which is a plan, not an enforced limit; 133 KB of SRAM_U stay free.

### The `diag` commands

Open the port with any terminal (`screen /dev/ttyACM0`, `minicom`, PuTTY; the baud rate means nothing). The banner and the prompt `o3> ` appear when the terminal raises DTR; typing works regardless.

| Command | What it prints |
|---|---|
| `help` | the commands |
| `version` | build identity (git hash, `-dirty`), USB VID:PID, REV, serial |
| `stage` | boot/run stage and module, uptime, reset sources, whether a crash record was found, and the start-up faults the image went on after (audio, display, app) |
| `cpu` | the audio load (`audio::diag::format_cpu`): cycles per stage, underruns, jitter |
| `mem` | the memory map and budgets (`audio::diag::format_mem`), the image's hot code and SRAM_U use, free SRAM |
| `crash` | the record the previous run left (kept for this run before main erased it) or "no crash record" |
| `usb` | state, address, DTR, line coding, bus counters, interrupt cost, EP0, CDC, MIDI, errors, the last stalled request |
| `log [n]` | the last n lines (default 20, at most 48) of the ring boot code and drivers write to; a pager that writes only what fits, so no line is lost to a slow host |
| `input` | live input events until a key is pressed; `input stats` the health counters |
| `mix` | the audio graph's meters (`audio::diag::format_mix`, OS-005b): per track, input, bus and master, the last 32 ms window's peak, the 1 s hold and the RMS in dBFS, and the count of windows that reached full scale; silent channels are left out, the master pair is always shown |
| `mon [off\|on\|0..100]` | the line-in monitor's level (OS-005b): the input strip's fader, through the engine into MAIN and CUE; 0 (off) at boot, `on` is 70 (-12 dB). The one command that changes the engine: a ParamChange from the main loop, smoothed over one block. Never with OUT patched to IN |
| `peek <hexaddr> [1..16]` | OS-HW1 bring-up: aligned 32-bit reads, one line a word; only inside the modules this image has clocked on (I2S0, SIM, MCG, DMAMUX, PORTA..E, GPIO, SCS and the eDMA's own registers: `usb/mem_access.h`), anything else is refused (a read of a gated module is a bus fault). **eDMA (OS-DIAG1)**: one window per register of the MK66, because the block is mostly reserved and a read of a reserved word faults (`0x40008008` did): CR `0x40008000`, ES `0x40008004`, ERQ `0x4000800C`, EEI `0x40008014`, INT `0x40008024`, ERR `0x4000802C`, HRS `0x40008034`, EARS `0x40008044` (one word each), DCHPRI `0x40008100..0x4000811F`, the TCDs `0x40009000..0x400093FF`; the write-only command bytes `0x40008018..1F` and every gap are refused. A multi-word peek stays inside one window (`peek 40009000 8` reads TCD0) |
| `poke <hexaddr> <hexvalue>` | OS-HW1: one aligned 32-bit write, I2S0 `0x4002F000..0x4002F1FF` only (for the MCLK bring-up), prints the value read back |
| `audio start` | OS-HW1: runs `start()` again from the main loop if audio is not running (a full retry: the hardware is stopped and re-programmed by every failed attempt), prints the result and, for a `ClockFault`, where it stopped (`mcr`, `mdr` or `boundaries`) with the raw MCR and MDR; on success the audio status polling starts and the audio boot fault and its LED code are cleared. Blocks about 0.7 s (the codec's ramp) |
| `rot <0..7>` | OS-HW1: writes REG_ROTATE now through the display driver (not saved; the `O3OS_DISPLAY_ROTATE` build option is the persistent one); "busy, try again" if a frame is in flight |
| `sd [mount]` | OS-007e: the SD card (slot, mount, identity, speed, counters, the last project load); `sd mount` initialises and mounts it again |
| `ls [path]` | OS-007e: a directory of the card, a pager (any key stops it) |
| `load [folder]` | OS-007e: reads an OG project folder (default `Workspace`) into the document |
| `inst <1..64>` | OS-007h: what instrument n plays: window, loop, root, rate, volume, a checksum of its first samples |
| `key <button> [tap\|down\|up\|hold <ms>]`, `pad <0..47> [tap\|down\|up] [vel]`, `enc <n>` | OS-HIL: input injection into the scanner's own event queue, see "Test host commands (OS-HIL)" |
| `state` | OS-HIL: the app as one `key=value` line (screen, page, overlay, the Pattern cursor, transport, tempo, instrument, pads, the last notification) |
| `shot [565\|8888 [w]] [pclk0]` | OS-HIL: the screen as framed base64 lines (`CMD_SNAPSHOT2` into the spare RAM_G, read back a few lines a pass) |
| `leds` | OS-HIL: the 48 pad LED levels the driver holds, one line |
| `reset` | SYSRESETREQ after the output has had at most 40 ms to leave |
| `fw` | OS-006c, the only command that writes the card: `fw` (write state, volume serial, `Firmware/` in directory order, the upload state, the last result), `fw put <name> <bytes> <crc32hex>`, `fw rm <name>`, `fw arm <name> <crc32hex>`, `fw update <token>`: see "Firmware update over USB" |

`tools/o3console.py` (stdlib, POSIX) is the host side: `o3console.py --port /dev/ttyACM0 version usb "log 30"` sends one command per argument, waits for the prompt after each and prints the cleaned reply
(`--list` finds the port, `--self-test` runs it against a pseudo-terminal that plays the unit; ctest `mk66_o3console_selftest`). `--put` and `--update` drive `fw` (below); it flashes nothing itself.

### Test host commands (OS-HIL: `key`, `pad`, `enc`, `state`, `shot`, `leds`; the PC tools `o3shot.py`, `o3audio.py`, `o3hil.py`)

What an agent (or a person at a desk) needs to see that the unit works without touching it. Level B; the `shot` path went through the firmware reviewer. Nothing here is maintainer gated except by the `fw` interlock below.

**Input injection.** `key <button> [tap|down|up|hold <ms>]` (a button by name, any case: `f1`..`f8`, `play`, `shift`, `master`, ... the simulator's names from `app/buttons.h`, or a code 0..32), `pad <0..47> [tap|down|up] [vel 1..127]`, `enc <n>`
(n detents, -64..64, not 0, one encoder event). The events go through `Scanner::inject`, into **the queue the key chips feed**, stamped by the same rule and in the same order, so bindings, gestures and actions see a finger's events; an injected pad also lights its
LED (it joins `pads_held()`). A tap is down now and up 40 ms later, a hold up after its time (at most 10 s; **`hold` answers `ok key <n>` at once and the release follows `<ms>` later, so a script waits that long before it reads the result: `o3hil`'s `name:ms` token does, a bare console `key up hold 1500` followed by a `state` read at once sees only the press and its first auto-repeats (v0.6.0 ODD-01)**; the auto-repeat is the recogniser's own clock, `core/gesture.h` kRepeatGapMs: 401 301 201 151 101 81 ms, the same for an injected hold as for a finger); the releases wait in a table of 8 (`usb/hil_inject.h`, run from `console::service()` every pass, with or without a terminal open). Each injection is one line of
`diag log` (`inject key 23 tap`), so a person at the unit can tell it from a finger. **Refused while an `fw` update is armed or a put / arm / hand-over runs** (`ERR refused: an fw update is armed or running`); a release that came due meanwhile is sent when that ends, so no
key stays held. Replies: `ok key 23`, `ok encoder -3`, `ERR unknown button`, `ERR the input queue is full, try again`.

**`enc accel [off | <t1> <t2> [gap_ms]]`** (OS-AUDIT-28, E-GLOB-11 / E-LOAD-09) is no injection: it shows and sets the encoder's acceleration in lists (`app/enc_accel.h`: File, Sample load, Modules, the module picker; never a value, the pattern or a Shift step).
A run is the detents of one direction with less than `gap_ms` (5..250, default 60) between two reports; a report moves its detents x1, x4 once the run before it had `t1` detents (default 6), x8 from `t2` (default 16); `off` is a row a
detent. Runtime only (a reset goes back to `6 16 60`); the numbers are to be tuned on the unit, `enc accel 6 16 60` restores the defaults.

**`state`** is one line: `screen=PATTERN page=0 mode=3 overlay=NONE pattern=5 track=0 row=12 col=NOTE playing=1 seq=1 tempo=12800 instrument=1 pads=NOTES shift=0 order=- play_pattern=5 play_step=3 posts=2 sel=- msg=SAVE_TRACK_01_DONE` (`app/state_line.h` lists the fields;
`posts` counts notifications since boot and `msg` is the last one's text with `_` for spaces, so a poller sees a repeat). **`leds`**: `leds online=1 errors=0 reinits=0 lit=3 sync=1 levels=<96 hex digits: pad 0 first, two digits a pad>` (the staged level; the driver applies its gamma;
`sync=1`: the chip holds all of it).

**`shot [565|8888 [w]] [pclk0]`: the screen.** (`pclk0`, OS-SIMKEY2: REG_PCLK is 0 while a band is taken and restored when it is done, aborted or faulted; off by default. The first captures read every RECTS fill at 0.545 of its colour while the LCD did not: if `pclk0` captures are exact the cause is the running clock, and it becomes the default.) The FT812's coprocessor takes the picture: `CMD_SNAPSHOT2` (format RGB565 by default, two bands of 240 rows; `8888` is the 32-bit format, six bands of 80 at its default `w` 1600) into the **spare RAM_G** (`ui/plot/ram_g_map.h` `kRamGShot`: from 0x71000, 572 KiB less the
DMA self-test's last 64 bytes; no bitmap, font or wave slot is touched). The default `w` is 800 for RGB565 and 1600 for 8888 (the chip counts `w` in 16-bit units; a 0.4.8 unit given `shot 8888` at w = 800 wrote the left 400 pixels at 1600 bytes a row, which read back as 3200-byte rows is a picture of two screen rows side by side with the rest white, OS-SIMKEY). The bands are sized by what the chip may WRITE, `rows * max(800, w) pixels * bytes per pixel`, so a larger `w` (H-1) makes more, shorter bands and can never run past the region; a `w` whose single row does not fit is refused and the main loop reads it back over the SPI. It is a stream of the shell: ONE call a pass of the main loop, at most **4 lines = 384 bytes** read from the chip per pass (about 0.4 ms of polled SPI at 18 MHz), only
when `Ft812Display::bus_idle()` says no frame or upload owns the SPI and the output has room for the lines (the CDC ring never drops any), so audio (interrupts and DMA), the display (its frames wait at most one pass) and the console never stall. A key press ends it (interruptible; a snapshot still queued is dropped, and so is one that times out, by resetting the coprocessor as the driver's own recovery does; a display that is set up again or stops mid-shot ends it with an `ERR`);
3 s without progress (the host does not read, the bus is never free) ends it with an `ERR`. The text (`usb/shot_stream.h`):

    SHOT BEGIN fmt=565 w=800 h=480 bytes=768000 line=96
    SHOT D 000000 <base64 of 96 bytes> <CRC-32 of those bytes>        (8000 lines; the offset is absolute in the image, 6 hex digits)
    SHOT END lines=8000 crc=<CRC-32 of the whole image> moved=0       (or: SHOT ERR <reason>)

`moved=1` says the app committed a frame while the bands were taken (they may belong to different pictures). **`o3console.py --shot out.png [--shot-format 565|8888 [--shot-width N]] [--shot-retries 2]`** (`--shot-width` needs `--shot-format`; at most 5 retries) collects the text, checks every line's CRC and the whole picture's, asks again for a damaged or
moving picture (all attempts moving: "UI animated, partial shot", the last picture kept, status 4), converts RGB565 / ARGB8 to RGB and writes the PNG (`zlib` + `struct`, `o3shot.py`). `o3shot.py compare unit.png golden.png [--tol 24] [--aa 1] [--max-bad 0.005] [--region x,y,w,h] [--ignore x,y,w,h] [--diff out.png]` compares it with the simulator's picture
(`o3sim --scenario S --out DIR` writes `DIR/<snap>.png`, 800 x 480 RGBA): a pixel matches within `tol` per channel (RGB565 loses up to 8 levels, the chip dithers) or, for the anti-aliased edges, when each picture has a like pixel among the other's neighbours within `aa`
pixels; it passes when at most `max-bad` of the compared pixels are left. Regions pick what is compared: a unit with a project loaded differs from the demo in its data, not in its chrome (the first scenario compares the key-cap row, `0,458,800,22`). The same compare is the basis of
OS-SYS's Shift + Power screenshot-against-golden.

**Shift then Power: the screenshot (OS-SCREENSHOT, `docs/specs/OS-SCREENSHOT.md`).** `core::PowerHold` reports `Result::screenshot` once when Power goes down (debounced) with Shift already down; `power::device::take_screenshot_request()` hands it to `appimg::pass()`, which arms the `console::ScreenshotService` (`usb/screenshot_service.h`, its state and 2 KiB piece buffer in the SDRAM window, nothing in SRAM). The picture is the first frame the app commits after Shift and Power are both up (plus 50 ms for the swap; 300 ms if the app draws nothing). The service shares the console `shot`'s `BandReader` (`usb/shot_stream.h`): `CMD_SNAPSHOT2` in RGB565 into `kRamGShot` in two bands of 240 rows, each read back over the SPI in pieces of 2 KiB, one piece a pass and only when `bus_idle()`. While it runs `pass()` draws no app frame (`frames_held`), so both bands are one display list; audio, the display driver and the console go on, and `console::hold_input(frames_held)` makes the app's input a `usb/input_hold.h` `InputHold` (between the key scan and the app, inside the console's `TeeInput`): presses and encoder turns are dropped, the releases of keys already down are kept (16 at most) and delivered after, so no note hangs (spec Q4). `ScreenshotService::cancel()` ends a request silently: `power_quiesce` (the shutdown's commit) and the display-failed and app-failed returns of `pass()` call it, so no temporary file or card claim outlives them. The service clears the console `shot`'s `pclk0` flag (`ShotPort::stop_clock(false)`) when it starts a capture; `screenshot_port()` is a plain getter. The bytes go to `Screenshots/_o3save.tmp` through the save layer's `FatStorageWriter` (path guard: `Screenshots/SHOT_nnnn.bmp`, a Temp then the NewOnly rename) as a 66 byte BMP header (16 bit, `BI_BITFIELDS`, top-down, 800 x 480) and the 768,000 snapshot bytes as they come. The next free name is searched two lookups a pass from the number after the last one taken or found taken (RAM, 1 after a boot). Refusals (strip `NO SCREENSHOT: ...`): no card, the card on the computer, not writable, `card_busy()` or a project load, a console `shot` (which in turn answers `ERR shot: a screenshot is being saved`), one already going, 9999 files. Aborts (card fault, a display set up again, 3 s without progress) remove the temporary file; the card claim (`screenshot_has_card`) holds the hand-over off while the file is open. The simulator runs the same service over its frame buffer (`Simulator`, scenario `e2e_screenshot`).

**`o3audio.py`**: `record [--device auto|hw:3,0] [--seconds 3] [--channels N] [--save out.wav] [checks]` (without `--channels`: the capture channel count the device itself reports in `/proc/asound/cardN/stream*`, 8 when that cannot be read: a boot presents "8 OUT / 2 IN") runs `arecord` (alsa-utils, 16-bit) on the unit's USB AUDIO device (`usb mode audio`; `usb profile` gives 2, 8, 10 or 11 channels; `auto` picks the `arecord -l` entry that names
`Track3r`), and measures a channel: peak and RMS in dBFS, DC, silence (RMS below -60 dBFS), clipping (samples at the rails, runs of three), and the pitch (Hann-windowed 8192-point FFT, parabolic interpolation, only when the line stands 12 dB out of the noise). Checks: `--expect-pitch HZ
[--cents 40]`, `--expect-silence`, `--not-silent`, `--min-rms`, `--max-peak`, `--no-clipping`, `--channel N`; exit 0 / 1 (a check failed) / 2 (no capture). `analyze FILE.wav` does the same on a file.

**`o3tone.py`** (OS-USBRECQA): the other direction of `o3audio.py`, a known tone from the PC into the unit's USB AUDIO return, for the scenario that records from it (`qa_usbrec`, QA-REC-034: Sample Rec with SOURCE USB). `play [--player auto|pw-play|aplay] [--device NAME] [--hz 1000] [--db -12] [--seconds 300]` plays a sine, the same on both channels, a whole number of cycles per second at 44.1 kHz (the loop has no step), in a loop of `pw-play` (PipeWire) or `aplay` (alsa-utils) on the device that `pw-dump` / `aplay -l` name `Track3r`, found at run time by name and never by a card number; `list` shows what it found; `gen FILE.wav` only writes the file. o3hil has no step that plays audio and none that runs a program on the PC, so it is started BY HAND in a second terminal before `o3hil run qa_usbrec`, and left running. Exit 0 / 2 (no player, no device that looks like the unit, the player failed). `--self-test` uses stand-ins for the players (ctest `mk66_o3tone_selftest`); nothing is played.

**`o3hil.py`**: the scenario runner. `o3hil.py run play_meters_vu --port /dev/ttyACM0 [--audio-device auto] [--sim build-os-host/sim/o3sim | --golden-dir DIR] [--expect-volume XXXX-XXXX] [--report r.json]`; `list`, `validate`, `--self-test`. A scenario is a JSON file in `tools/hil/` (YAML is not
stdlib): steps `cmd` (expect / expect_not / within), `keys`, `pad`, `enc`, `wait`, `state` (poll until the pairs match), `mix` (tracks that sounded, limits, a drop or rise against an earlier snapshot), `leds`, `voices`, `shot` (+ `compare`), `audio`, `card`, `run` (a snippet `_name.json`), `finally`
(always run). The shipped scenarios: `boot_version`; `play_meters_vu` (load t-v16, play, 12-track metering, the VU pads, the Pattern key row against the simulator's golden, stop); `mixer_volume_meter` (the Mixer's master level reaches the MAIN meter: the param-ring regression);
`song_play_from_row`; `sample_load_audition`; `perform_scene`; `save_test_card` (File F5 SAVE AS; checks with `fw` that a card is mounted and not write protected, and that it is the `--expect-volume` one when that is given, before it sends a key; any SD card, no marker file; then waits up to 200 s for the project's own `SAVE TRACK NN DONE` (the samples' `SAVE INSTRnn DONE` come first) and lists `Projects/Track NN` for a non-empty `project.o3pj`).
A scenario whose needs (`audio`, `sim`, `card`) are missing is skipped. The runner sends no `fw` command but the read-only `fw`, and none of `reset`, `poke`, `audio` (validation refuses them). **One session owns the port at a time.** Against a pseudo-terminal that plays the unit,
`--self-test` runs every step kind and the shipped scenarios' syntax (ctest `mk66_o3hil_selftest`, `mk66_o3shot_selftest`, `mk66_o3audio_selftest`, `mk66_o3tone_selftest`, `mk66_o3disk_selftest`).

**The generated scenarios (`tools/hil/gen_hil.py`, OS-HIL2).** Beside the hand-written ones above, `tools/hil/` holds 32 scenarios made by `python3 tools/hil/gen_hil.py [outdir]` (stdlib only; it rewrites every one of them: edit the generator, not the JSON): `nav_all_screens`, `pattern_edit_undo`, `song_edit`, `pad_modes`, `perform_full`, `sample_edit_undo`, `record_screen`, `modules`, `tracks_9_12`, `mute_solo`, `slices_pads`, `mixer_sends`, `config_display`, `persistence`, `limiter_*`, `cpu_play`, `stress_voices` and the `cfg_clock_*` setups. `_load_tv16` unmutes the tracks the project saves muted and `_play_song` plays the whole song; `start_on_pattern` ends with `cpu reset`, so under/over-run counts start from zero. Order: the ones that check underruns or clip counters since boot (`limiter_*`, `stress_voices`) after a `reset` of the unit. The scenarios that write the card (`persistence`, `config_display`, `cfg_clock_*`) start with a `card` step (a card is mounted and not write protected; the `--expect-volume` serial too, when it is given). `--expect-volume` is optional (OS-ANYCARD).

**The USB disk scenarios (OS-USBDISK U1d: `qa_usbdisk`, `qa_usbdisk_rw`, `qa_usbdisk_chord_*`; Linux only).** The step kind `{"disk": "enumerate|mount|read|write|remount|eject|gone"}` drives the PC side through `tools/o3disk.py` (stdlib; `/sys`, `/proc/mounts`, `udisksctl`; its own ctest `mk66_o3disk_selftest`). Rules: the scenarios `need` `"card"` and `"disk"`; they run only on Linux (`--expect-volume XXXX-XXXX` is optional); on Windows and macOS they are skipped as untested (not run, not failed, also with `--strict`). Before it reads or writes a file the tool proves that the block device is the unit's USB disk (`16d0:0e9f`, bcdDevice `0002`), that the computer sees it as removable (sysfs `removable=1`) and not mounted at `/`, and, when `--expect-volume` is given, that its volume serial is that one; any SD card, no marker file (OS-ANYCARD); the only writes are `<mount>/o3-usbdisk-test/o3-usbdisk-<n>m.bin`, deleted at the end; the raw block device is never opened. The runner cannot power-cycle the unit: a person holds Shift + F1 + F7 at power-up, then starts `o3hil.py run qa_usbdisk --port ...` (with `--expect-volume XXXX-XXXX` to check the card). The chord cases are one boot each (`qa_usbdisk_chord_exact`, `_shift_f1`, `_shift_f7`, `_f1_f7`, `_all4`). `qa_usbdisk` only reads; `qa_usbdisk_rw` (`--var SIZE_MB=100` for the release pass) is the writable half: the boot disk is exported writable since the writable step (the log says `usb: disk opened, writable`, the computer sees `ro=0`; a key on the unit is then two presses while the computer may be writing, QA-GLB-023, by hand). The unit-side bench is `diag bench disk` / `diag bench disk reset` (bench images; reads, and the write arm on any card the Tracker owns inside one scratch file, Q14; docs/specs/OS-USBDISK.md section 11.1).

`persistence` saves the open project with SAVE AS under the OS's own proposal, the first free `Track NN` (no fixed name, so no `--var` and nothing to run out of: the old `0hilp<digit>` names did, the OS has no erase), reopens it from the first row of the RECENT group (checked with `sel` to be a `Track NN` before F1), checks the values are back, and hides the scratch project with DELETE (an empty `.deleted` in its folder; the folder, about 8 MB, stays on the card: remove the `Projects/Track NN` folders that hold a `.deleted` from a computer now and then). Any scenario may declare `"vars"` (defaults) and use `${NAME}` / `${NAME-1}` in its strings; `gen_hil.py` writes them. Helpers, not scenarios: `hil/midi_clock_tool.py` (`measure`, `send --profile 120:10 --start --stop|--continue`: the unit as clock master / slave over a raw ALSA MIDI node) and `hil/usb_return_test.py` (the USB return's latency from the PC side).

**Hardware checks (none of this has run on a unit; the scenarios' expected values come from reading the app, not from a run):**
H-1 `shot` returns a picture at all: RGB565 800 x 480, two bands (it does, on a 0.4.8 unit). `shot 8888` needs `w` = 1600 for the 800-pixel rows: the Programmer's Guide does not
explain it, and a 0.4.8 unit given w = 800 returned the left 400 pixels (OS-SIMKEY): the default is 1600 now, and on 0.4.8 `--shot-format 8888 --shot-width 1600` is the same request; check one `shot` in each format after the update. H-2 the screen glitches for a frame or two while a band is taken (the Guide says to set REG_PCLK to 0 around CMD_SNAPSHOT2; this one does not by default, so that the display driver's swap watchdog cannot see a stopped clock): confirm it is
harmless, and if a band is wrong, say so. H-3 one `shot` takes about 1.5 to 3 s over the CDC (1 MB of text) with the audio playing, `diag cpu` shows no underrun and the UI stays alive. H-4 `key`, `pad`, `enc` act as the matrix does (a tap on Play starts the sequencer; the pad LED lights on `pad N down`).
H-5 `leds` against the pads actually lit. H-6 every scenario once, and the values they assume (t-v16's tracks, the Song cursor, the Sample load listing, the Mixer's column count) corrected.

Maintainer-gated (ADR 0011 item 3) until OS-006c, now only through `fw` (the maintainer's decision in CLAUDE.md, "Firmware delivery"): the bootloader pin and `_fwinfo` have no other writer. The crash record is erased by main after its LED code, so `diag crash` shows it for the run after the crash only.
The log is 1,024 records of 64 bytes in the `diag_log` region of the SDRAM (session only; SDRAM does not survive power-off); it starts in `main` (the SDRAM test overwrites everything before that).
A command is the action `diag.<name>` (`core::action_id`), so a keymap or a MIDI-learn binding can name it later.

## MIDI drivers (OS-005c2, `src/midi_din.cpp`, `usb/src/usb0.cpp`, the logic in `midi` and `seq`)

The DIN port and the USB MIDI endpoint bound to the MIDI timing core of OS-005c. **What every interrupt does is in `midi/include/o3os/midi/din_io.h` and `usb_bind.h`, tested against fakes (`tests/midi_io_test.cpp`); what is here is the registers** (`midi_config.h`, checked by `tests/mk66_midi_config_test.cpp`). The sync end to end (Off, Master, Slave) is `seq::MidiLink` (`seq/include/o3os/seq/midi_link.h`), the engine's event source.

| What | Where | Source |
|---|---|---|
| UART4, 8N1, **31,250 baud**: SBR 180, BRFA 0 from the 90 MHz **bus** clock, exact | BDH/BDL/C4, `midi_config.h` `uart4` | NXP's device header |
| FIFO off (`PFIFO` = 0): the data register in front of the shift register is the whole transmit buffer, whatever depth the chip has (the card's NXP quote says one) | `midi_din.cpp` step 3 | NXP's device header |
| RX pin PTE25 (Teensy 34): ALT3, pull-up, passive filter. TX pin PTE24 (Teensy 33): ALT3, high drive, slew-rate limited. TX is handed from the GPIO that idles it high (the quiet-pin table) to the UART only after `TE` is set, so the line does not glitch | `board.h` `kMidiRx`, `kMidiTx` | NXP's device header |
| PIT0, one-shot at the bus clock (one tick = two CPU cycles, a byte 28,800 ticks), re-armed by the interrupt for the emitter's next wake (`next_wake`), floor 90 ticks (1 us), ceiling 100 ms | `midi_config.h` `pit` | NXP's device header |

**Where each interrupt sits** (the table of ADR 0013 D1, `hal/irq.h`, rows in `irq_map.h`): UART4 RX_TX 48, USB0 64, PIT0 96, render 128. The three above the render are the only readers of `midi::TimeRef`, which the render alone writes.

| Context | Does | Shares |
|---|---|---|
| UART4 RX (48, SRAM_L) | stamps the byte with `CYCCNT` as its first act; S1 then D (clears the flags); a byte with a framing, noise or parity error is dropped and counted, an overrun is counted and the byte kept; at most 4 bytes | the DIN `midiRt` ring: **this interrupt is its one producer**, the render its one consumer |
| USB0 (64, flash) | before the device logic, when a SOF is pending: `UsbMidiOut::frame()` into the to-host ring, so the same pass arms the bulk-IN buffer with it; after it: the from-host ring into stamped bytes of the USB `midiRt` ring | the to-host ring (this interrupt writes, the device logic of the same interrupt reads), the USB `midiRt` ring (this interrupt the one producer), `UsbMidiOut` (this interrupt the one consumer) |
| PIT0 (96, SRAM_L) | stops the timer, maps `CYCCNT` to the timeline (`TimeRef`), if `TDRE` writes the byte `DinEmitter::poll()` returns, arms the timer for `next_wake()` or leaves it off | the DinEmitter's rings: **this interrupt is the one consumer**, the render the one producer |
| render (128) | publishes `(sample, cycle)` once a block, takes the settings (sequence lock), drains both `midiRt` rings, steps the sequencer, routes its MIDI events, hands the ports what is due in the next 2.9 ms, pends PIT0 if it handed the DIN ring anything | |

**The time base.** `audio_block_heard_at()` (`audio.cpp`) is the cycle counter value at which the first sample of the block being rendered is *heard*: the boundary stamp of its render (`Pipeline::render_stamp()`) plus the TX ring's render-ahead (`midi_cfg::kAudioLeadCycles`, two blocks = 522,240 cycles with the default three slots, one block with two: OS-CPU10 slice 3). MIDI out is aligned to the sound that way (a byte due at a sample leaves with that sample), and an incoming clock is placed on the timeline where the sound of that instant is. The I2S and codec pipeline after the TX ring is not in it (a constant the user's offsets cancel; an HW-B row). A render that starts one boundary late can read a boundary stamp one block newer than the block it serves (the note in `audio/slot_ring.h`), which skews that one block's map by a block; under that condition the audio has already underrun.

**Memory (measured, `-Os`, against the previous integration build):** flash +14,580 B (of which 2,560 B is SRAM_L code copied from flash: the emitter's `poll` / `next_wake`, `UsbMidiOut::frame`, the two handlers), `.data` +16, SRAM_U `.bss` +21,736 (`MidiLink` 21,728: the emitter's rings 5.3 KB, USB out 5.2 KB, the router 6.5 KB, two `midiRt` rings 4.2 KB, the loop 0.4 KB), SRAM_L +2,560. `tools/check_ramfunc.py` finds nothing new in the hot path (the two sites it reports are `VoicePool::fenv_eval`'s lambda, OS-007m, present before this slice).

**Hardware checks (HW-B; nothing here has been run on the unit):** DIN clock jitter, idle wire and busy wire, on a logic analyser on PTE24 (the targets: 50 us and 320 us); UART4's real transmit depth and the gap between the bytes of a message with commit depth 1; PIT0 interrupt-entry jitter under load (a GPIO toggle, `midi_io::tx_stats().rt_late_max`); RX interrupt latency (the stamp is the entry); lock time and phase error following a reference master per port; the two input-latency constants (DIN 320 us, USB 0.5 ms: open item O24); the first note after a Start; USB MIDI as the host sees it; the sign of the offsets and the advance limit (an OUT advance is honoured up to about the render-ahead, 1.45 ms, because a byte cannot leave before the sequencer has made it); audio-to-MIDI alignment on the scope; a USB pass delaying a clock byte (PIT0 cannot preempt USB0).

## USB AUDIO mode (OS-006d, `usb/src/usb0.cpp`, the logic in `usb/device.h` + `iso_audio.h`, `audio/stem_map.h`)

ADR 0012 slices S3 (engine side) and S4 (audio). **Since OS-USBAUDIO the unit comes up in AUDIO** when the device setting USB AUDIO is On (the default; Config > USB, row 17 of the settings file): the port is started after the card is read, so it enumerates once, in the mode the setting says, with the boot profile 8 and the saved USB RETURN (a card that is absent or unreadable gives the defaults: AUDIO). Before it every boot was STANDARD. **`usb mode audio|standard` stays runtime only and does not write the setting** (a reboot comes up as the setting says; Config's USB AUDIO, press to apply, is the persistent way). The profile and the stem map are the project's `USBS` part (the mode byte of it no longer decides: the setting does). `diag usb ...` is the interface for the mode, the profile and the map (the Config > USB page has USB AUDIO, USB RETURN and RETURN LEVEL):

| Command | Does |
|---|---|
| `usb mode [standard\|audio]` | no argument: the mode and profile now. With one: re-enumerates (drain 120 ms so the reply leaves, D+ off for 700 ms, descriptors rebuilt, D+ on). The terminal drops and must be reopened. **Runtime only (OS-USBAUDIO):** the device setting USB AUDIO is not written, and the main loop does not copy the mode back to it; a reboot comes up as the setting says. |
| `usb profile <2\|8\|10\|11> [out\|duplex]` | the channel layout (10 = T1..T8 + master, 8 = T1..T6 + master: Windows shared-mode safe). In AUDIO it re-enumerates; in STANDARD it only sets what the next AUDIO attach uses. A new profile resets the map to its factory slots. `4s` (post-pan stereo stems) is refused: the StemTap is mono, pre-pan. The optional word also sets USB RETURN (OS-USBRET): `usb profile 10 out` is **"10 OUT"** (capture only, return off), `usb profile 8 duplex` is **"8 OUT / 2 IN"** (return on); without it the setting stays. Every boot presents "8 OUT / 2 IN". |
| `usb return [on\|off]` | USB RETURN (OS-USBRET, the device setting of the same name, Config > USB): no argument shows it. Off drops the OUT interface, the return terminals and the feedback endpoint from the AUDIO descriptors, so a host that cannot fit playback next to ten capture channels (a hub's transaction translator, ADR 0012 amendment 5) opens capture alone. A change re-enumerates in AUDIO; the main loop copies a console change to the setting and a Config change to the driver. |
| `usb map [reset \| <slot> <sources>]` | the slot map: `usb map 8 T8+T12`, `usb map 1 -`. Sources: `T1..T12`, `ML`, `MR`, `SA`, `SB` joined by `+`. Applies at once (a block boundary). Tracks with no slot are heard in the master pair. |
| `usb ep0` | OS-USBDISK U2a: the first 32 events of endpoint 0 in the last DISK attach (boot disk or `disk share`), one line each: `#NN +<us since the one before> reset / setup type req value index len / arm-in / in-done / out-done (buffer, bytes, DATA0/1, odd or even descriptor, PID) / stall set / stall sent (the FIRST handshake of a stall; the later ones, the host's retries, are not listed) / stall-cleared (the SETUP that ended the stall: the stall lives from `stall set` to this line) / setup-armed (after a stall: the receive descriptors owned, the freeze released or kept, and TXSUSPENDTOKENBUSY read back after the CTL write: `set` = the controller is still deaf to tokens) / address / error / idle`. The last line is `stall retries N`: the STALL handshakes after the first of each stall (the host retried a token and was stalled again instead of NAKed; a lower bound, one flag bit covers several handshakes), so N greater than 0 shows the retry reading holds. Kept in RAM across the rollback to the previous layout, so it is read once the console is back; empty before a DISK attach. 388 B of SRAM, filled by the USB interrupt only while the DISK set attaches. |
| `usb audio [reset]` | mode, streams, measured rate (frames per SOF, ppm against nominal), feedback value and trim, IN packet sizes and fill, OUT fill, under/overruns, missed frames. `reset` zeroes the stream counters. |

**What runs where.** The USB interrupt (64) does everything isochronous, in one pass of `UsbDevice::isr()`: endpoint 4 (stems IN, return OUT, one ENDPT4 register, EPHSHK clear) and endpoint 5 (feedback IN). IN: ONE packet is armed at a time, built (interleave of at most 45 frames x 11 channels into the single packet buffer) at the completion of the previous token and at a SOF when nothing is armed; OUT: both descriptors always armed (2 x 192 B), each re-armed as its token completes after the frames are written to the return ring; feedback: one 3-byte packet armed at a SOF when none is pending, so the host reads a value at most one interval old. Isochronous descriptors have DTS off and DATA0 (`bd::arm_iso`). SET_INTERFACE alt 1 takes the endpoint's descriptors back and starts the stream; alt 0, a new configuration, a bus reset and a mode change stop everything and turn the engine's taps off (the stream hook sets `StemTap::set_enabled` and `Graph::set_return_enabled`: single atomic stores). The SOF stamp is `CYCCNT` read when the interrupt handles the SOF (after any tokens already pending), the frame number is FRMNUML/FRMNUMH.

**Ring contracts.** Stems: the render writes whole 128-frame blocks into `StemRing` (never waits, drops the oldest when the USB side lags: `in_dropped_blocks`), the USB interrupt is the single consumer (at most one packet per SOF). Return: the interrupt writes at most 48 frames per OUT packet into `ReturnRing` (drops the oldest: `out_dropped_frames`), the render's `ReturnReader::pull` is the single consumer (64 frames per block; silence while priming and after an underrun, counted: `out_underruns`). The render (128) never preempts the USB interrupt (64). A disabled return costs the render one atomic load; an idle stream costs the render nothing (the tap is off).

**USB RETURN** is heard on the outputs by a route of its own (OS-006e, ADR 0012 amendment 6), at the device setting RETURN LEVEL (Config > USB, 0..100 on the mixer law, 100 = unity = default) times the master level, independent of the input strip, the master DRY level and DRY MUTE: a DAW that plays into the Tracker is heard at the level it sends with the monitor at 0. **NO LOOPBACK:** it is added at the very end of the render, in no capture: not in the stems (Master L / R included), RecTap Master (a resample), the MAIN meters, CUE or REC, and it bypasses the limiter (a loopback setting, device row 17, is a follow-up). The input strip is the line-in alone (it starts at level 0: `diag mon on`, 70, is -12 dB, and the master DRY level no longer scales it: a v7+ project's saved input level is 5.6 dB louder than in 0.4.6, which is less feedback margin for a microphone); while the recorder's source is the USB the strip carries the stream instead and the direct route ramps shut. CUE and REC are not routes of the return (TODO: a matrix row).

**The mode change** is a state machine run from `console::service()` (`usb0::service()`): nothing blocks the main loop. The descriptors are rebuilt in the same RAM buffers with the interrupt masked and `ControlPipe::rebind` (the pipe then ends like a bus reset). The CDC console, USB-MIDI and the OS-006c update path are present in both sets (CDC is interfaces 0,1 and the endpoint numbers 1..3 are the same in both; MIDI is interfaces 2,3 in STANDARD and 5,6 in AUDIO, where the UAC2 function takes 2..4 so Linux finds its terminals, OS-006d); the console treats a detach like a closed terminal. bcdDevice tells the sets apart (STANDARD 0x0001, AUDIO 0x0101).

**Memory (measured against the previous integration build, `-Os`, incremental):** flash +9,368 B (the stream and device code 3.4 KB, the `diag` text and commands 3.0 KB, the rest is library code that was unreferenced until now: interleave, rate estimator, feedback), SRAM_U `.bss` +3,792 B (the return ring 2,068, the OUT buffers 384, the IN packet 992, `IsoAudio` about 260, the configuration buffer 208, counters). **SRAM_U headroom over its 32 KiB floor is now 640 B** (it was 4,432 B): the next addition to SRAM_U needs a plan change (ADR 0013 D7) or a move to SRAM_L/SDRAM. SRAM_L +152 B (`Graph::add_return`).

**Hardware checks (S5, `hw-session`; nothing here has run on a unit):** see "Hardware-session checklist: USB AUDIO mode" below and ADR 0012 "Validation for the hw-session".

## Firmware update over USB (OS-006c, `update/`)

The maintainer's decision (`CLAUDE.md`, "Firmware delivery") lifts ADR 0011 item 3 for this one path: an image can be put on the card and the bootloader asked to flash it, from the host, over the `diag`
console. **Nothing here has run on a unit** (checklist FW-*). Spec and deviations: `docs/specs/OS-006-usb.md`, 006c. The first install of an image that has `fw` goes the old way, by card.

```
o3console.py --port /dev/ttyACM0 --put build/o3os.ptf --name o3os-0.7.ptf [--expect-volume XXXX-XXXX]
o3console.py --port /dev/ttyACM0 --update o3os-0.7.ptf --crc <the crc the put printed> --yes-update [--expect-volume XXXX-XXXX] --expect-version <build id>
```

### What the bootloader needs (all that is known) and what is not
A `.ptf` (Intel HEX) in `Firmware/` of the card; `Firmware/_fwinfo` holding exactly the bare file name (no folder, no newline, no NUL; a stale one is deleted first); then the bootloader request pin 81 = PTA1
(`board::kBootloaderRequest`) as a GPIO output (MUX 1, DSE, SRE) driven low. The unit flashed the named image after this hand-over (2026-10-12, below); that the bootloader
deletes `_fwinfo` afterwards is not observed here. **Unknown, so unverified:** what reacts to the pin (the hardware or the bootloader), whether it needs a reset from
somewhere else, whether the pad function this code restores when it releases the pin (the reset default, ALT7 JTAG_TDI with the pull-up: PCR 0x703, from the RM's table, not from `MK66F18.h`) is a good resting state, whether the bootloader reads a version from the image, (answered on the unit 2026-10-12: it **does** flash a `.ptf` whose name does not start `PolyendTracker_`: `Track3r_0.3.1b.ptf` was flashed through `fw update` and the unit came back as that build; the prefix is probably only a filter of the official update menu; not verified). **The hidden reset button flashes the first `.ptf` of `Firmware/` in directory order**: a broken
candidate that lands first makes the button reflash it, recoverable only by moving the card. `fw` marks that file with `*` and `fw put` prints `first-ptf <name>` at the end.

### The commands
| Command | Does |
|---|---|
| `fw` | write path (enabled, or why not), the card's volume serial (`XXXX-XXXX`: a host may match it against the card it means to write), file system, `Firmware/` there or not, write protect, `Firmware/` in directory order with sizes and `*` at the first `.ptf`, the upload state, the last result |
| `fw put <name> <bytes> <crc32hex>` | one `ERR <reason>` line on a bad name, an existing target, no `Firmware/` (a strict build also: no marker), size 0 or over 4 MiB, a project loading, no room; otherwise `ready`, then the receiver **owns the terminal** (no echo, no line editor: `Shell::start_raw`) and takes exactly `<bytes>` raw bytes, `+<received>` every 2 KiB (the host sends at most 4096 past the last one), and ends with `ok <name> <bytes> <crc32> image <len>` and `first-ptf <name>`; on the first error `ERR <reason> at <offset>`, the temporary file deleted, the rest of the declared bytes swallowed (the prompt returns when they are in, or after 5 s of silence) |
| `fw rm <name>` | the temporary file or any valid image name that does not look official, never a name starting `Polyend` (any case) other than the old dev namespace `PolyendTracker_0.`, never the last `.ptf` of the folder |
| `fw arm <name> <crc32hex>` | re-reads `Firmware/<name>` and re-validates it completely, checks the CRC, prints `armed <8 hex digits>`; the token is one use, expires after 30 s and is cancelled by any other `fw` command, a reset, or the terminal closing |
| `fw update <token>` | the token, the card's serial and the file's size must still be the armed ones, no project may be loading, and the file is **read and fully re-validated again** (the CRC of arm included); then: sequencer stopped, outputs muted, a stale `_fwinfo` deleted, a new one written (the bare name), synced, read back and compared, `update: handing over to the bootloader (<name>)`, the console's output drained (at most 40 ms), PTA1 driven low. 5 s later, if the chip still runs: the pin released (input, pad function restored to the reset default), `_fwinfo` deleted, `ERR bootloader did not take over`. The 5 s clock is the main loop's (`FwService::poll`), not the terminal's: closing the terminal does not leave the pin low |

**The client API (OS-FWCONFIG, new Community OS behaviour; the Config screen that uses it is a later slice).** `FwService` has a shell-free side for a screen: `ui_check_begin(name)` is `fw arm` without a CRC to compare and without a token, `ui_slice()` runs the 2 KiB slices of the check (and of the install), and `ui_install()` is `fw update` for the file that check passed (valid 30 s, dead once the card went to a computer or changed, re-validated completely again before anything is touched). They share `verify_slice`, `begin_handover` and `handover_check` with the console, so there is one hand-over and one 5 s fallback; `FwServiceSink` (`update/fw_sink.h`) puts the App's `hal/firmware.h` port over it. One owner at a time: a console command is answered `ERR busy` while a client operation runs, `ui_check_begin()` answers `Busy` while a console one does, and any `fw` command drops a check that waits for its install. The HIL key injection is refused while such a check waits. `platform/mk66/tests/mk66_update_client_test.cpp` runs the console's `arm` + `update` and the client's check + install on the same fakes and requires the same calls.

### The safety layers, in the order the spec ranks the goals
1. **Never hand the bootloader an image that could lock the MCU or not boot.** The flash configuration field and the image shape are checked on the host (`o3console` runs `check_image.py` first and refuses), on the device on the stream as it arrives
   (`HexValidator`), and again on the bytes **read back from the card** through a fresh validator, with the CRC-32 and the length compared; then again by `fw arm` before the token. `HexValidator` (`update/hex_validator.h`) applies `check_image.py`'s rules: record syntax and checksums,
   types 00/01/02/04 only (a start-address record is refused), EOF last and only line ends after it, addresses <= 0xFFFFF, no hole, `0x0..0x40F` present, SP exactly 0x20030000, a Thumb reset vector inside the image past 0x410, 116 vector entries (flash, or the SRAM_L code window
   `0x1FFF0000+0xC000` at an offset inside the image; the five reserved slots 0), and **the flash configuration field exactly `ffffffffffffffffffffffffdef9ffff` with FSEC & 3 == 2** (its own copy of the constant, `static_assert`ed against `flash_config.h`). One difference, on purpose: with O(1) state it needs data records in ascending, consecutive address order (what
   `objcopy -O ihex` writes), so it refuses a HEX that `check_image.py` would accept with its records shuffled (`HexOrder`). `fw_hex_cross.py` runs both on 160 files (every family of `check_image.py`'s own self-test: ctest `mk66_fw_hex_cross`) and, with `--hex`, on the real built image and 30 mutations of it, and requires the same verdicts.
2. **Never write outside `Firmware/`.** (OS-FWANY, maintainer decision 2026-10-13, and OS-ANYCARD, 2026-10-17: the marker gate that kept the writer to the test card is gone, build option included; any writable FAT card with a `Firmware/` folder takes the writer, and the host's `--expect-volume` is an optional check of WHICH card is written.) The path guard (`update/path_guard.h`: `Firmware/_o3up.tmp`, `Firmware/_fwinfo`, `Firmware/<stem>.ptf` with `<stem>` = `[0-9A-Za-z][0-9A-Za-z._-]{0,47}`, the suffix in any case, no `..`; removal only for the temporary file and names that do not look official: not `Polyend*` except the old dev namespace `PolyendTracker_0.`) is applied by the transfer and again, to the
   full path, before every write-class FatFs call. Every write needs a FAT12/16/32 volume (exFAT is refused), a medium that is not write protected, an existing `Firmware/` (the writer never makes a folder), and
   (No marker file is needed.) `fw` shows the volume serial so a host that wants to can insist on one card with `--expect-volume`, an optional check of `o3console.py` for `--put` / `--update`. Existing targets are never replaced (`f_rename` refuses, the check before it too).
3. **A half-written file is never a `.ptf`**: the upload lives in `Firmware/_o3up.tmp` and is renamed only after the read-back verification; a failure, a timeout, a pulled cable or card deletes it; a stale one is replaced by the next upload.
4. **The update is never started by a stray line**: arm, then a token that is unpredictable enough (cycle counter, uptime, a hash of the unique id, a counter), single use, 30 s, bound to the card and the file.
5. **The read-only guarantee of OS-007e stays true for everything but one narrow writer**: see "SD card, As built in OS-006c" (layers, `check_layers.py` rules 6 and 7, the 16-command allowlist).

### How it runs (real time)
Everything is the main loop's, in slices: a pass takes at most one buffer (2 KiB, one `f_write`) of the upload or one 2 KiB slice of a read-back; the display, USB and the audio path (interrupts and eDMA) are unaffected by the card. **Blocking calls that are not sliced:** `begin_upload` reserves the room (a seek past the end
of a file open for writing allocates its clusters; R0.15a's `f_getfree` would scan the whole FAT on every call) and may scan the FAT for free clusters (bounded by the FAT's size, at most about a second on a full 14 GB card); `commit` and `write_fwinfo` are a few sector writes. One `CMD24` is one
block: the data phase, the CRC status and the busy are each bounded by `sd_policy.h` (`kWriteDataUs` 250 ms, `kWriteBusyUs` 500 ms, `kWriteStatusUs` 500 ms; at worst 1.7 s for one block of a card that hangs, `worst_write_block_us()`, tested < 2 s), then the call fails and the upload is aborted. Receive back-pressure
is the pump's (`diag_pump.h`): the receiver takes nothing while the output has less than 32 bytes of room, the receive ring fills, the interrupt NAKs the host.

### The SDHC write phase (image only, `sdhc_bus.cpp`)
CMD24 (R1, DPSEL set, DTDSEL clear, one block of 512 bytes), then per watermark group of 16 words: wait for `PRSSTAT[BWEN]` (bit 10, `MK66F18.h` `SDHC_PRSSTAT_BWEN_MASK`), write 16 words to `DATPORT` (little endian); `WML[WRWML]` is 16 (set with RDWML at power-up); then the transfer-complete flag and `PRSSTAT[DAT0]` high (the card's programming busy);
a data CRC error (the controller raises it for a CRC status token other than "accepted"), a data timeout or any error flag resets the data circuits and sends CMD12, as a failed read does. The card logic then asks CMD13 until READY_FOR_DATA in the transfer state. **None of this can be proven off the unit** (the soc test pins the bit values against the NXP header; the sequence is the RM's, written from its register tables): checklist FW-4.

### Cost (clean `ci.sh mk66` image, default build, `-Os`, against the previous integration build)
Flash +17.0 KB (220,520 to 237,524 B; FatFs write functions +6.1 KB; the service, store, validator, guard, texts and the SDHC write phase about 10.9 KB, of which about 1 KB is message text), `.bss` / SRAM_U +5.1 KB (92,664 to 97,776 B: the service's 2 KiB transfer buffer, the validator's 1 KiB head, two FatFs file objects with their sector buffers, one directory and one `FILINFO`). No SRAM_L, no SDRAM, no eDMA channel, no interrupt.

### Tests
Host (ASan, UBSan) and QEMU: `mk66_update_hex_test` (every record defect, every byte of the flash configuration field in four ways and every bit, holes, SP / reset / vector / SRAM_L cases, every chunking, the CRC), `mk66_update_path_test` (accept and refuse tables, every character, every one-character mutation of an accepted path),
`mk66_update_store_test` (the store on a FAT32 volume: upload, read-back, rename, refusals that write nothing, the tree outside `Firmware/` byte-identical by hash, the last `.ptf`, `_fwinfo` bytes, a full volume, a failing medium, a pulled card), `mk66_update_transfer_test` (the real shell and pump with a fake store: a good upload with acks and one write per pass, every error path, discard, idle timeout,
back-pressure, packet sizes, tokens, the hand-over order and the 5 s fallback, a terminal that goes away), the card tests (CMD24 and CMD13 per block, write protect, CRC status, timeout, a stuck card, a pulled card), the allowlist test (16 commands), `check_layers.py` self-test (rules 6 and 7), ctest `mk66_fw_hex_cross` (host) and
`mk66_o3console_selftest` (put, injected ERR, refused name, wrong volume, update with a new and an old version, a bootloader that does not take over, a unit that does not return).

### Deviations from the spec text
- `FirmwareStore` is in `platform/mk66/update/firmware_store.h`, not in `hal`: nothing above the platform has a use for it and its contract is the bootloader's. `hal::storage::Storage` is unchanged.
- The FatFs-backed store, the service and the validator are in `platform_mk66_pure` (so the host and QEMU tests run them); only the `FwHost` (transport stop, mute, drain, PTA1, entropy) and the SDHC write phase are image-only.
- "Insufficient free space" is found at `begin_upload` by reserving the clusters (see above), not by `f_getfree`.
- The HEX validator is stricter in one respect (records in ascending order).
- exFAT volumes are refused for writing (the exFAT write path of FatFs is not exercised by the tests).
- The slot's write-protect contact is not read (none is known to be wired; `write_protect_switch()` is false): only the CSD's own bits refuse a write. A card with its tab set is **not** protected by this code.
- Acknowledgements every 2 KiB (the spec says at least every 4096); a write that fails is not retried inside a call.
- `fw update` also checks the card's serial and the file's size again (an addition); `fw` commands other than `update` cancel a token, `update` with any argument spends it.
- The real-image cross-check is a command (`fw_hex_cross.py --hex <o3os.hex> --tool <o3os_fw_hex_verdict from a host build>`), not a ctest of the mk66 build (that build cannot run host tools); the ctest covers the corpus.
- `appimg::stop_transport()` is new in `app_main.cpp` (the composition root), the one edit outside the update and storage trees besides the console table.

## Saves: the card's write side (OS-007n, `save/`, the job in `app/atomic_save.h`)

`hal/storage.h` stays read-only. The saves of the OS (the native project, a WAV, the device settings) go through a second contract, `hal/storage_write.h` (`StorageWriter`: gate, make_dir, create a temp, write, finish, rename, remove), guarded by `hal/save_guard.h` and implemented over FatFs by `save/src/fat_storage_writer.cpp`, the second and last file that may name a write-class FatFs call (`check_layers.py` rule 6; rule 8 says who may include the contract: the save job, the service, the writer and this image's composition root).

**The gate.** Every mutating call is refused unless a FAT12/16/32 volume is mounted and not write protected (`NoCard`, `Unsupported` for exFAT, `WriteProtected`): checked before every call and again before every chunk, so a card pulled or swapped mid-save stops the write. **Any such SD card is saved to: no marker file, no build option** (OS-004d, 2026-10-12, made the old `O3OS_SAVE_REQUIRE_TEST_CARD` gate off by default; OS-ANYCARD, 2026-10-17, removed it with `ReadOnlyCard` and its "CARD IS READ-ONLY (NO TEST-CARD MARKER)" notice). Always refused: exFAT, a write-protected card, and anything the path guard does not name. The firmware writer has no marker gate either (OS-FWANY, OS-ANYCARD).

**What may be written** (the guard, `tests/save_guard_test.cpp` walks it): `Projects/<name>/project.o3pj` (+ `.bak`), `Samples/<name>.wav` (never replaced), `Settings/device.o3st` (+ `.bak`), the temp `_o3save.tmp` in the same folder, and the folders on the way. `create` takes a temp only; `rename` has three pairs (temp to target, target to its own backup, backup to its own target); `remove` takes a temp or a backup only (never a target, never a folder, never a user's file); nothing formats; and `remove` refuses (`Damaged`, "card needs a check") a file whose first cluster another name of its folder uses, the cross-link a cut inside `f_rename` can leave.

**The save** (`AtomicSave`, a `core::Job`: one directory operation or one chunk of 0.5 to 8 KB per step): write `_o3save.tmp`, sync, read it back through the read-only side and compare size and CRC-32, then (Replace) drop the old `.bak`, rename the file to `.bak`, rename the temp to the file; a new file (a WAV, Save As) is one rename that refuses an existing name. What a power cut leaves, step by step, is the table at the top of `atomic_save.h`: the user's data is whole in the file or in the `.bak`; the next save heals (a missing file with a `.bak` gets it back). Tests: `atomic_save_test.cpp` (every cut of a replacing and a new-file save, every single failing call, on an in-memory card that applies the real guard), `save_fatfs_test.cpp` (the same over FatFs on a volume built in memory, with a block device that loses power at every block and one that fails one block), `mk66_save_writer_test.cpp` (the writer alone, also on QEMU).

**Where it lives.** `SaveHost` (the writer, the service with the job, the native writer, a 1 KiB settings buffer: about 7 KB) is the next part of the sample arena's SDRAM tail after the recorder; SRAM_U gained 8 bytes (the App's pointer). File F4 SAVE / F5 SAVE AS, Record F7 SAVE WAV, Sample edit Shift + F7; the device settings save themselves 1.5 s after the last change, quietly, and load at boot (read only). New is not built (it needs the platform's document reset).

### Save policy (maintainer decisions of 2026-10-12)

The saver removes its own old `.bak` (the previous version of the file it replaces, once the new one has been written and read back; it never removes a target, a folder or a file that is not its own temp or backup) and the device settings save themselves 1.5 s after the last change: both are **maintainer approved**. The marker gate no longer exists (OS-ANYCARD). A card that is swapped or re-mounted with another volume serial in the middle of a save stops it (`NoCard`): `make_dir`, the first call of every save, pins the serial and every later mutating call compares it.

### Hardware-session checklist: saves (OS-007n, for `hw-session`)

0. First image with saves (any SD card; no marker): items 1 to 3 below. Also: swap the card for another one in the middle of a large save: the strip fails, the second card gets nothing from that save.

1. Any writable FAT card: File F5 makes `Projects/Track 01/project.o3pj`; F4 again keeps the old as `project.o3pj.bak`; the PC reads both; the strip says `SAVE TRACK 01 DONE`.
2. A write-protected card (the card's switch): F4, F5, Record F7, Sample edit Shift + F7 all say `CARD IS WRITE PROTECTED` and the card is byte-identical afterwards (compare a file listing and hashes on a PC).
3. Pull the card in the middle of a save of a large project (many instruments): the strip fails or stalls, nothing else happens; put it back: the PC finds `project.o3pj` whole (or the `.bak`), maybe a `_o3save.tmp`; the next F4 cleans it.
4. Record a take, F7: `Samples/REC_001.wav` plays on the PC at the right length; Sample edit, an effect, Shift + F7: `SMP_001.wav` has the edited audio.
5. Change a Config row, wait 2 s, power cycle: `Settings/device.o3st` exists and the row is as left.
6. Time the F5 of a big project with the log (`JobStats::max_step_us`): the cost model `AtomicSave::kBytesPerUs` is provisional.

## Power switch (OS-POWER, `power/`, the logic in `core/power_hold.h`; docs/specs/OS-POWER.md)

**Slice P1: the hold, the line, the shutdown stages and a restart. Slice P2 (below): the real off, in very-low-power RUN, with a polled wake.** New Community OS behaviour; no parity is claimed (Polyend's quick-start documents press-to-start and hold-to-off with an on-screen bar, **[public]**: documented, not verified; the official hold time, shutdown order and off state are not known here). Evidence of what is below: **[code]** unless marked.

- **The pin.** PTC3 (Teensy pin 9, `board.h` `kPowerSwitch`), a GPIO input; the board constants `kPowerActiveLow` (default true) and `kPowerPull` (default the internal pull-up: a pin nothing pulls could float low for 2 s and commit a shutdown) in `board.h` are the two unknowns (**open**, measurement M4: `diag power` reads the pin with no pull, the pull-up and the pull-down, idle and pressed). It is read ONCE per pass of the main loop (`appimg::pass()` -> `power::device::pass()`), nowhere else while the app runs, and never from the idle hook (which runs only inside `boot::delay_ms()`); two samples can be seconds apart (the card mount blocks up to 3 s, the codec unmute 0.2 s), which `core::PowerHold`'s gap rule handles (a sample more than 250 ms after the one before is a new start: the debounce, a hold in progress and Booting's 100 ms release all start again).
- **The hold** (`core::PowerHold`, header only, host-tested for both polarities in `tests/power_hold_test.cpp`): Booting (not read until the app has the screen and the level was released 100 ms: a Power held through the boot, or a switch that always reads pressed, never arms; one log line `power: held or stuck since boot` at 5 s), Armed, Holding (debounce 30 ms both edges; the line shows from 150 ms at its true fill, `Commit` once at 2000 ms), Spent (Shift down at the press: reserved, nothing shown; Shift pressed during the hold cancels it until Power is released), Committed (one-way: a release does not cancel). A tap of 150 ms to 2 s shows the hint for 1.5 s.
- **The line** (`app/screens/overlays.cpp` `render_power`, drawn after the overlays): a full-width line on the top edge of the status strip, 4 px, `Sel` on `Chrome` (the pair `ui/theme.h` already lists), a one-pixel `ChromeDim` rule under its whole length; no new colour, no new token. The strip text is on the info line in the look of the notification strip. `App::set_power()` takes the fill and the text once a pass; the Hold text becomes `UNSAVED WORK - LET GO TO KEEP IT` while `SaveService::dirty()` or a take exists.
- **The shutdown** (`power/src/power_service.cpp`, a state machine over a `PowerHost`, host-tested with a recording fake in `platform/mk66/tests/mk66_power_service_test.cpp`; the image's host is `power/src/power_host_mk66.cpp`): stage 0 refusals (firmware update, export, writable disk; a refusal calls nothing), then A quiet (`appimg::power_quiesce`: the power-off latch FIRST, the input locked at the tap, the transport and the live notes stopped, `App::power_begin`), A2 mute (`audio_stop()`: the codec's DAC through its soft ramp, the key LOW, the outputs muted, the analogue stage powered down: `sgtl5000.h` `run_stop`, tested on the register model; then the I2S and the two eDMA channels stop), B card writes (the device settings are flushed at once, a save in flight is finished for 5 s then cancelled unless it is committing, 8 s hard cap), C unmount (`card_busy()` false, or the hard cap, then `storage::device::release()`; the MSC close in the boot disk mode), D USB (`usb0::detach_for_power()`), E lights (`PadLeds::park()`, the backlight to 0 and held dark, the FT812 in power-down, PD# LOW; after it `appimg::pass()` does not touch the display), F the key LOW, F0 a stable release of Power (100 ms), then `boot::reset_chip()`. **After the commit** a global deadline (10 s from stage A) abandons every stage that has not finished and goes to F (the card is released only if it is not busy); F0 is outside it.
- **The audio path changes in P1** (not the block callback, the DSP or the voices): `Mk66Audio::stop()` is new (there was no stop path: `stop_hardware()` ran only when `start()` failed), `main.cpp` takes no audio stall verdict and no pattern once the latch is set (otherwise the stopped block counter latches `kAudioFault` after 300 ms), and `mk66::open_wanted()` has a sixth argument, `powering_off`, that makes it false whatever the others say.
- **Locks after the commit** (D2): the key scan's events are read and dropped at the console's tap, `poll_midi_cc` returns, `poll_card` holds, the boot disk's "any key restarts" is off, and the diag shell refuses every command but `help version stage log crash power state cpu mem` with `ERR powering off` (a bit per command: `command_table.h` `kPowerReads`, pinned by `mk66_usb_text_test`).
- **`diag power`**: the pin (level, in the three pulls), the hold's state and counters, the shutdown's stage; `power press|release|pin` inject the switch in place of the pin; `power off [dry]` runs the shutdown (in P1 `off` and `dry` are the same: the stages 0 to E, then a restart after the release; the stage lines are in the diag log as `power: <stage> +N ms`: `log` right after `off`, the console ends at USB off). **The first unit run is `power` and `power off dry` with no button held** (QA-GLB-113); the bootloader's reaction to a button held at a reset is **open** (section 6 of the spec): F0 never restarts while Power is down.
- **Slice P2, the VLPR off** (`power/src/power_off.cpp` the pure sequence and the poll loop, `power_host_mk66.cpp` `Mk66OffPlatform` the registers, `clock_config.h` the VLPR clocks, `tests/mk66_power_off_test.cpp`): after F0 a real off (a hold, or `power off`) runs `off_prepare()`: interrupts masked and SysTick stopped (from here no SDRAM access: the code is in flash, the object on the stack, the marks in SRAM_U), a running watchdog (never today) is a restart, the USB module's IRC48M off (`usb0::irc48m_off()`, RM 6.7.2), ONE `SIM_CLKDIV1` step in HSRUN that doubles every divider (core /2, bus and FlexBus /4, flash /14: RM 16.4.3.3 limits a clock change in HSRUN to a factor of 2 per step, so the x11 drop to the crystal is not made there), HSRUN -> RUN, then in RUN MCGOUTCLK PEE -> PBE (the crystal), `SIM_CLKDIV1` to core, bus, FlexBus /8 (2 MHz) and flash /16 (1 MHz) with a read-back, PBE -> FBE -> BLPE, RUN -> VLPR with `PMSTAT` waited, then the pins (the board's quiet pins but the FT812's PD# stays LOW, the I2S master clock pin LOW, the 39 SDRAM pins LOW) and, only with `kOffCutsSdram`, PTA24 LOW last. Then `off_poll()` reads PTC3 every 1 ms: a stable release of 100 ms, then a press of 30 ms, then (`kWakeAfterRelease` in `power_off.h`, **true** by default until QA-GLB-103 has passed on the unit, because the bootloader may react to a button held at a reset) a stable release again, is `boot::reset_chip()` (a full boot); `kOffCutsSdram` is **false** by default (PTA24 stays HIGH: the external pull-up of PTC3 may hang on that rail), and `pins_quiet()` ends by putting PTC3's clock gate, direction and pull back; a stuck-pressed switch or a glitch never wakes. **Any step that fails or runs out of polls (`kOffPollLimit` = 200 000) resets the unit instead of stopping it**, leaving `last off RESTARTED at step: ...` for `diag power` after the boot. **The record of the last shutdown** (`power/.../power_record.h`, 28 bytes in the noinit SRAM with a magic and a check word, written by `PowerService` as it goes and by the off platform for the off's result) is what a run with no hands reads: `power` after the restart prints the last stage reached, the ms from A of each stage, the notes (cancelled save, caps, deadline), the last refusal and the result of the off. It survives the reset the way the crash record does (the same unverified premise: the SRAM holds through SYSRESETREQ); the boot path never reads it. `diag power off dry` and a build with `-DO3OS_POWER_VLPR_OFF=OFF` keep P1's restart (the rollback of the decision "if the wake fails on the unit, ship P1's restart-off only"). **Every register fact of the sequence was checked against NXP's reference manual and data sheet on 2026-10-17** (the repository holds no copy; the pages are in the table in section 4.7 of the spec, which also records the two changes that check made: the factor-of-2 step out of HSRUN and the IRC48M off); none has run on a unit yet. The poll loop busy-waits on the cycle counter at 2 MHz; a wait-for-interrupt wake is a follow-up if M3 says the draw matters.
- **Sizes and cost**: see the report of the slice (`ci.sh mk66`: FLASH, SRAM_U, SDRAM deltas); one GPIO read a pass, nothing in the render or an interrupt.
- **Unit checks** (the `hw-session` list): `qa_power.json` (injected hold, the stage lines of `log`) and the manual rows QA-GLB-100..116 of `docs/qa/catalogue/global.md`: the polarity and pull (M4), a real hold, a hold through the boot, Shift + Power, a save in flight, an export, the click on the line out, the USB detach, the restart after the release; P2: `qa_power_off.json` (`power off`, then the draw and the press, QA-GLB-103/117) and `qa_power_last.json` (run after `qa_power` or `qa_power_off`, once the unit has booted: it reads the record of the last shutdown, with no hands).

## Watchdog re-enable point

`main.cpp` carries a marked block where a later slice turns the watchdog back on, with the idle loop as the place that feeds
it. It is **not** done here: this tree has not checked the watchdog's clock source selection and timeout units against the
reference manual or a unit, and a wrong guess would be a reset loop on the first boot. Verify first (a deliberate watchdog
reset on a unit, and the crash record surviving it).

## Hardware-session checklist: first boot of the skeleton (for `hw-session`)

Images: **A** = default build (SDRAM full test on, drill off), **B** = `-DO3OS_MK66_FAULT_DRILL=ON -DO3OS_MK66_SDRAM_FULL_TEST=OFF`.
Each needs `fw-gate-check` and the maintainer's decision first; B only after A passes. Not for the maintainer's only SD card.

**Read before flashing, safety first.** These images have **no update path**: the app-side update (Config >
Firmware) does not exist and pin 81 is never driven. (Images from OS-006c on have `diag fw`; the first install of such an image still goes this way, by card: "Firmware update over USB".) (Until OS-006b the images had no USB either; since then the default image enumerates as
`16d0:0e9f Track3r` with a serial port and MIDI, see "Hardware-session checklist: USB and `diag`"; it still has no update and no mass-storage function.) The way back is the **hidden reset
button with exactly one known-good `.ptf` in `Firmware/`** on the test SD card (`docs/hw/tracker-hardware-notes.md` section 3,
the flashing-safety rules Gates B and D): stage and test that recovery card before flashing. With the display image (OS-007c) the
screen is initialised and shows the pattern view (DSP-1); an image from before OS-007c leaves it dark, which is expected. Stop at the first abnormality (heat, smell,
repeated resets, noise) and recover.

| Id | Image | Steps | Expect | Capture |
|---|---|---|---|---|
| MK66-1 | A | Power up on stable USB power (the flashing-safety rules Gate B). Film the LED from power-on for 30 s. | LED on steady for T1 seconds, off, then a 100 ms pulse each second, for ever. Never code 2 or 3. | video; T1 = power-on to first heartbeat |
| MK66-2 | A | Where the LED is not visible: probe the PTC19 test point (Teensy pin 78) with a scope or logic probe. | idle HIGH, 100 ms LOW pulses at 1 Hz | scope screenshot |
| MK66-3 | A | Headphones on the lowest level at the output, line out to a recorder, during power-up and 5 minutes of idle. | silence: no pop, no hiss change (the amplifier stays muted: PTD6 low) | recording |
| MK66-4 | A | Time 60 heartbeat pulses from the video. | 60.0 s, within 0.1 %: the SysTick reload assumes 180 MHz, so a wrong core clock moves the period | frame times |
| MK66-5 | A | Five cold power cycles (not the hidden reset button: it starts a firmware flash). | the same sequence every time; T1 stable | five T1 values |
| MK66-6 | A | Ten minutes idle, then feel the module and the regulators. | not warm: the core idles in WFI | note |
| MK66-7 | A | If a scope is on hand: CLKOUT (PTA6) frequency. | 90 MHz (FlexBus clock) | screenshot |
| MK66-8 | B | Power up; film the LED for 60 s. | heartbeat for about 10 s; one reset (the heartbeat stops and restarts); no visible pause for the SDRAM tests (wiring only, milliseconds); **7 flashes, twice**; then the heartbeat for ever | video |
| MK66-9 | B | Power-cycle B again; film 40 s. | the drill repeats once per power-up (10 s heartbeat, reset, 7 flashes twice, heartbeat): the record is cleared after each report | video |
| MK66-10 | A | Idle for 5 minutes with the board on the bench: look and listen for the quiet state: screen, pad LEDs, line out and headphones, the radio, the MIDI DIN OUT (a MIDI monitor if one is at hand, only with the gear disconnected from anything that matters). | screen: the pattern view since OS-007c (dark with a skeleton image from OS-007b: FT812 in standby, PD# released; DSP-1 reads the picture), pad LEDs off, silence (also no radio noise: the radio is held in reset), MIDI out idle (no bytes) | note, photo; any lit LED or sound is a finding |
| MK66-11 | A | If a scope or logic probe is on hand: levels at the MCU pins or their test points: PTD6, PTE26, PTE27 (LOW), PTD15, PTD11, PTE24 (HIGH), PTC19 (HIGH, 100 ms LOW pulses). | as listed: the pin table above | note per pin; a pin at the other level is a finding |

Reading MK66-8: **7 flashes** = the record survived the reset and its fields are right (UsageFault, UNDEFINSTR, stacked PC in
flash, module `0xD1`, stage Running): crash-record round trip proven. **6 flashes** = a valid record but the wrong fields
(for example HardFault instead of UsageFault: the fault enable did not take): report it with the video. **No flashes after the
reset, only the heartbeat** = no valid record: SRAM did not hold through SYSRESETREQ (or the write failed): ADR 0002
validation 6 fails, move the record to the VBAT register file. If the device does not come back from the drill at all, that is
the finding (the handler's reset): recover and report.

Not checkable without a console (record for the `diag` slice): the reset-cause bits, the crash record's contents beyond "valid
and as expected", the stack high-water mark.

Needing confirmation, from the pin table (MK66-10, -11): that the FT812 PD# line exists and that holding it released is harmless
(ADR 0011 amendment 1 asks whether it is wired); that PTE27 is the radio's reset on this board; that the pad-LED driver and the codec come up dark and silent without any I2C
writes; that PTE24 held HIGH is the idle state of the MIDI DIN output.

Hardware questions this session answers: SRAM across SYSRESETREQ (MK66-8); the true core clock (MK66-4, -7); the full SDRAM
test time and that the wiring tests pass (MK66-1, -5); whether PTC19 is a visible LED (MK66-1/-2); that mute holds (MK66-3).
Open for later sessions: SRAM across watchdog and lockup resets; the LMEM cache and SDRAM alias (HW-D); `_fwinfo`.

## Hardware-session checklist: HW-A, the audio skeleton (for `hw-session`)

Images (each needs `fw-gate-check` and the maintainer's decision first; never an agent's flash):

| Image | Build | What it is |
|---|---|---|
| **A** | default | the audio path runs; the outputs are muted at reset and open once at boot (OS-OUTON, after OS-005d's first-Play open: once the SD card is mounted, the boot project has loaded and the audio has run clean for 0.6 s; the amplifier key's click is that one), then stay open until power-off or a reset. Play, Song Play and live notes never wait (the demo pattern of the demo kit, or the project, plays through the sampler voices); no press, no sound |
| **B** | `-DO3OS_AUDIO_TEST=ON` | A, then after 0.6 s of clean audio the unmute and the test tone: the **left channel alone at 1 kHz for about 3 s, then the right channel joins at 1.5 kHz** (so a swapped or mis-mapped pair is visible), -20 dBFS |
| **C** | B + `-DO3OS_RATE_44100=OFF` | the pre-1.0 clock, kept to compare: MDR 16/255, 44,117.647 Hz. Images A, B and D-F run the D-OS-8 clock (MDR 196/3125, exactly 44,100 Hz) since v1.0.0; the retune trial below (C-1, C-2) passed on the unit on 2026-10-23, when it was the option's ON image. The option changes one constant (`core/audio_clock.h`); the host, QEMU (USB included) and image checks all pass at both rates |
| **D** | B + `-DO3OS_MK66_FAULT_DRILL=ON` | a deliberate fault 10 s after power-up: a reset while the amplifier is open (the fault handler drops PTD6 first, OS-OUTON: `boot::reset_chip`) |
| **E** | B + `-DO3OS_SGTL5000_I2S_CTRL=0x0030` | the I2S control word with SCLKFREQ = 64 Fs (see E-1) |
| **F** | B + `-DO3OS_AUDIO_TEST_STALL=ON` | 20 s after the tone starts one render overruns on purpose (3.5 blocks): three underruns, the mute, the recovery. The LED then shows the sticky fault code, as it should |

Order: A, then B, then C-F. Same recovery precondition as the first-boot checklist (one known-good `.ptf` on the test card, the hidden reset
button). **Safety first:**
- keep headphones off the ears for the first run of B and every later image, start with the line out into a recorder or scope;
- **never halt the core with a debugger or probe while the amplifier is open** (B to F, from about 1 s after power-up; image A too since OS-OUTON, from
  the open at boot, after the project load): the eDMA keeps running when the core is halted (`DMA_CR` EDBG), so it replays the last block for ever, a ~690 Hz
  buzz at tone level. Read the status over a probe without halting (background memory access), or halt only with the output unplugged;
- stop at the first abnormality (heat, smell, resets, a pop, any sound from A).

| Id | Image | Steps | Expect | Capture |
|---|---|---|---|---|
| A-1 | A | Power up on stable USB power. Film the LED for 30 s; time power-on to the first heartbeat T2. | the LED as in MK66-1, then, about 0.6 s later than T1 (the codec's 500 ms ramp), the heartbeat for ever. Never code 10 (audio failure), 2 or 3 (the display's 4 and 5 are its own, see the display checklist) | video; T2 - T1 |
| A-2 | A | Line out into a recorder (or the lowest headphone level on a sensitive monitor) from before power-up, through 10 minutes of idle. | **silence, no pop at power-up** and none while the audio path starts (the amplifier key stays LOW and the codec outputs stay muted until the open); **one click when the outputs open at boot** (OS-OUTON: after the project load, `diag log` says `audio: outputs open`; note its peak and its time from power-on); the idle noise floor after it (A-8) | recording; mark the power-up, the first heartbeat and the log line |
| A-3 | A | Leave it 10 minutes after the first heartbeat, then another 10 with the hand on the case and the USB cable wiggled (supply noise). | the heartbeat the whole time: **no sticky code 11 or 12**, i.e. zero underruns, late renders, missed boundaries, I2S/eDMA errors, governor overruns, and the block clock never stopped | note: time of any code 11/12 |
| A-4 | A (probe; read without halting: the amplifier is open since OS-OUTON) | Read the status (`o3os_audio_status_ptr`, layout above) after 10 minutes. | boundaries about 413,000 (600 s / 1.45 ms); **renders about equal to boundaries: the render runs through IRQ 64** (a render count of 0 means the software-interrupt path does not work); underruns, slips, hw faults 0; `period min..max` 261,120 +- a few cycles (the fractional MCLK divider: wider means the MCLK jitters); render max (cycles), per-stage maxima (input, drain, render, strips, buses, master, convert, taps, commit), start latency max = the render jitter | the 38 words; compare the render figures with the budget 156,672 (60 %) |
| A-5 | A | A signal at the line input (a phone at moderate level) with the status read as in A-4. | `input_peak` follows the signal (0 to 32,768), and is small with nothing connected: the RX DMA, the ring and the slot choice work | `input_peak` readings |
| A-6 | A (probe; the amplifier is open since OS-OUTON: halt only with the output unplugged) | **The real TX FIFO margin.** Halt the core at 1,000 random moments (the DMA keeps running, EDBG) and read `I2S0_TFR0`, the TX FIFO's read and write pointers (the difference modulo the pointer range is the fill), and `I2S0_TCSR` FEF. | with the watermark at 1 the fill sits at 0-2 words, so the margin before an underrun is about 2 words = 11-22 us (a word is 11.3 us); `FEF` never set. A fill that is often 0 at the moments the DMA is idle means the margin is thinner than estimated: then try `kTxWatermark` = 4 (audio_config.h) and repeat. Also read the receive FIFO | the 1,000 fills as a histogram |
| A-7 | A | **OS-OUTON, by ear (the maintainer's bug: a click on Play).** Line out and headphones, volume as for music. Power on and wait for the heartbeat and `audio: outputs open` in `diag log`. Then Play, Stop, Play, a pad hit, a MIDI note, Song Play (Song screen), Stop; then `diag reset`; then power off. | **one click at power-on** (when the outputs open at boot, a second or more after the screen appears, after the project load); **no click on any Play, Stop, live note or Song Play**; `diag reset` or power-off: **at most one click** (the amplifier key drops first, then the codec; what the board's output stage does while PTD6 floats during the reset's milliseconds is D-1's question). A Play pressed in the first second after power-on may wait for the open (`play: waiting for the outputs to open`), then starts. | the maintainer's ear; a recording marking each press |
| A-8 | A | OS-OUTON: the idle noise floor with the outputs open (line out into a recorder, nothing playing, 60 s, then with the screen busy: scroll a pattern). | compare with the floor before the open (A-2): the open must not raise it audibly; note any hiss that follows the screen or the USB traffic | recording, RMS before and after |
| B-1 | B | Line out (both channels) into a scope and a frequency counter, headphones off the ears. Power up; wait for the heartbeat; the tone starts about 0.6 s after it. | no click when the amplifier opens (the DAC ramps first, the key is last); **the left channel starts alone, a 1 kHz sine fading in over about 50 ms, the right channel silent for about 3 s, then the right joins at 1.5 kHz**. Left tone on the left jack: a swapped pair would show the sequence on the other jack. Amplitude about 0.13 V peak to peak per channel [est: -20 dBFS of the 1.3 V full scale of line-out level 0x1D] | scope capture of the start, of the right channel joining, and of both steady |
| B-2 | B | Frequency counter on each channel (1 s gate or longer), 3 readings each. | **left 1000.00 Hz and right 1500.00 Hz, each within 0.1 %** of the nominal. The tone's increment comes from the rate constant, so the reading is nominal x (true sample rate / the constant's rate): it measures the real MCLK divider against the constant. With the 4080/1 constant (image C), 999.6 Hz and 1499.4 Hz would mean the real rate is 44,100 Hz (x 44,100 / 44,117.6) | the 6 readings, the counter's time base noted |
| B-3 | B | Look at each channel on an audio interface's spectrum for 30 s. | one line per channel (1 kHz left, 1.5 kHz right), harmonics below -70 dB (the sine's error is 3.5e-6, the 16-bit floor is -96 dBFS), no spurs, no dropouts, **no 1.5 kHz on the left jack or 1 kHz on the right** (crosstalk or a mapping error) | spectrum screenshots, noise floor |
| B-4 | B | Headphones held near, then on a low-volume ear only after B-1..B-3 are normal: the headphone path. | a quiet tone (the headphone code 0x40 is -20 dB of the nominal level, about -40 dB below full scale: a few millivolts); left first, then both; no thump when the headphone opens | note: level by ear; Vrms on a scope across a 32 ohm load if one is at hand |
| B-5 | B | 10 minutes, then A-3's second 10. | the heartbeat the whole time, the tone clean, no gaps | note |
| B-6 | B | Power-cycle five times, watching and listening for the first second. | the same sequence every time; **never sound before the codec ramp is over**; no pop on any cycle | note per cycle |
| B-7 | B (probe, **do not halt**) | Status after 10 minutes, as A-4, read without halting the core. | as A-4 (renders about equal to boundaries); render max in the order of 15,000-20,000 cycles [est] (about 6-8 % of the block: two tones through the graph, table G-2); the start latency max is the render jitter | the 38 words |
| C-1 | B (the retune trial, passed on the unit 2026-10-23: true clock 44,100.00 Hz against 44,117.58 Hz on the old clock; the 1800000/441 constant is the default since v1.0.0) | As B-1 and B-2 with the 1800000/441 constant. Power up; the heartbeat must appear (no code 10: the codec accepts 11.2896 MHz and 44.1 kHz exactly). | **left 1000.00 Hz and right 1500.00 Hz again**: the constant is now 1800000/441, so that reading means the true rate is exactly 44,100 Hz, i.e. MDR 196/3125 does what ADR 0012 amendment 2 says (1000.4 Hz and 1500.6 Hz would mean the clock is still 44,117.6 Hz) | counter readings; a scope on MCLK (PTE6) or LRCLK if reachable: 11.2896 MHz / 44,100.0 Hz |
| C-2 | B | The spectrum of B-3 and the noise floor against image C (the old clock), then 10 minutes as B-5. | no loss of lock (no gap, no silence) over the 10 minutes and a noise floor within 3 dB of image C's: **the retune trial passes**. If it fails the engine keeps 4080/1 and ADR 0012 decision 2, option A, applies unchanged | spectrum, floor in dBFS for B and C |
| C-3 | B (probe, do not halt) | The status after 10 minutes. | `period min..max` 261,224 +- a few cycles (the block at 44,100 Hz is 261,224.5 cycles) | the 38 words |
| C-4 | B and C | **Spurs of the fractional divider.** The MCLK divider alternates between two integer periods in a fixed pattern (15/16 source clocks for 16/255; a much longer pattern for 196/3125), which jitters MCLK by one source clock (5.6 ns) at the pattern's rate. Spectrum, 0-22 kHz, 30 s, tone on, linear averaging off, for B and C. | no spur above -80 dBFS that B does not also show, and none more than 6 dB above B's floor at the same frequency; list every spur above -90 dBFS with its frequency (the pattern rate and its multiples folded into the audio band tell which divider made it) | the two spectra overlaid, the spur list |
| D-1 | D | **A reset while unmuted.** Scope or recorder on the headphone output and the line out, a scope on PTD6 (the amplifier key, at its net or test point) if reachable, from 5 s before to 10 s after the fault at 10 s. | the tone until the fault; at the fault PTD6 is driven LOW by `reset_chip` (OS-OUTON) just before SYSRESETREQ, then high impedance for the milliseconds of the reset until `early_pins` drives it LOW again (whether the board's output stage mutes then is not known, this row measures it) and the outputs go quiet **without a loud thump** (note its peak on both outputs); the fault handler drops the amplifier key ONLY (OS-OUTON: no I2C there), so the codec stays unmuted and keeps its registers through the reset (listen for it between the key dropping and the next boot's mute), and the next boot's first four writes (the mute block of the plan) are what silence it; the LED then shows 7 flashes twice (the crash record) and the tone comes back once, after its 0.6 s | recording of both outputs, PTD6 trace, the thump's peak |
| E-1 | E | **The 32 Fs / 64 Fs question.** Image E has `CHIP_I2S_CTRL` = 0x0030 (SCLKFREQ 64 Fs) instead of 0x0130 (32 Fs). The data sheet says, for slave mode, "this field must be set appropriately to match SCLK input rate (0x0 = 64Fs, 0x1 = 32Fs)": the K66 drives 64 Fs, so 0x0030 is the data sheet's letter; 0x0130 is what PJRC's Audio library writes and what is proven on this board. B-1 to B-3 and B-5 with image E, the same recorder settings. | the same tone, no new artefact, no pop at start; a noise floor and distortion no worse than B's. If E is cleaner, the default changes to 0x0030 (one constant in `sgtl5000.h`, its test says why); if E is worse or equal, the proven 0x0130 stays and the question is closed | spectra of B and E overlaid; start-up recording |
| F-1 | F | **Left and right after a forced underrun.** Image F, both outputs on a scope and a recorder, from the tone starting to 40 s. At 20 s after the tone starts the render overruns for 3.5 blocks (5 ms). | a brief stale-block replay or gap, then silence for about 50 ms (the mute, 32 clean blocks to lift), then the tone fades back in on **the same channels: 1 kHz on the left jack, 1.5 kHz on the right**, no swap (counter on each, scope on both, before and after). The LED turns to the fault code 11, for ever (intended). Probe, not halting: underruns 3, max run 3, mute events 1, late drops 1, muted 0 again, slips 0, **hw faults 0** (the I2S FIFO never starved: the engine underran, the DMA did not) | scope captures before and after; the counters |

Reading B-2: the tone is a Q32 phase accumulator per channel with the exact increment of its frequency at the build's rate constant, so the
counter's error is the clock's (the 16 MHz crystal's tolerance, tens of ppm), not the generator's. A reading off by about 400 ppm is the
4080/1 against the 44,100 Hz clock, or the reverse.

Hardware questions this session answers: that the SGTL5000 comes up muted and stays muted until `unmute()`; the power-up order's silence on
this board; the I2S frame format and the 32/64 Fs word against this codec; left and right on the right channels, also after an underrun; the
eDMA rings, the slot choice and the real FIFO margin; the render through IRQ 64 and its cost and jitter against the 60 % governor; the
behaviour of the output stage across a reset; the codec at 196/3125 and its spurs.

### HW-A, the fixed graph (OS-005b): rows G-1 to G-7

Same images as the table above (the graph runs in every one of them; with image A nothing is routed, the input strip sits at level 0 and the render is idle), and the same safety rules. Image B now plays the tone **through the graph**: the left tone is track 1 panned hard left, the right
tone track 2 panned hard right, both to MAIN at unity; at unity the block is bit for bit the one the skeleton made, so rows B-1 to B-7 hold unchanged (B-7's render figure grows by the graph, below). From 10 s after the tone starts, image B repeats a 30 s script of three
phases: the tones fixed (10 s), a **level sweep** of both tracks (100 down to 20 and back, a triangle over 10 s), a **pan sweep** (track 1 hard left to hard right and back, track 2 the other way round, 10 s). Each value is a ParamChange from the main loop every 50 ms, smoothed over one
block by the engine; image F (stall) does not run the script. `diag mix` and `diag cpu` (USB console, OS-006b) read the numbers; without a console use the probe layout above. In these images nothing consumes the scope and spectrum rings, so they are off and cost nothing, every meter is measured (the mask is all on) and the level law is our 1.1.14-table law (`Og1114`, this OS's model).

| Id | Image | Steps | Expect | Capture |
|---|---|---|---|---|
| G-1 | B | After the tone has run with both channels (about 6 s), `diag mix` three times a few seconds apart (before the script's 10 s mark). | T01 and T02 **peak -20.0 dBFS** (the tone's 0.1 of full scale), **rms -23.0 dBFS** (a sine is 3.01 dB under its peak), hold the same, clip 0; MAIN L = T01 and MAIN R = T02 (hard pans: nothing of track 1 on MAIN R); MST L and MST R the same; no other channel listed (IN, CUE, SNDA, SNDB, REC absent). The dB figures are steady to the printed decimal from window to window (the integers behind them are exact over 1,408 frames and move by an LSB or two with the phase of the sine) | the three outputs |
| G-2 | B (probe or `diag cpu`, do not halt) | After 10 minutes: `diag cpu`, the stage columns (last/max cycles), and the render max against the budget 156,672 (60 % of the block). | **Budget table, a cycle model [est], not a measurement.** The model: the loops of the built image (`arm-none-eabi-objdump`) with LDR 2 cycles (1 after another load), STR 2, LDRD/STRD 3, a taken branch 3, anything else 1; steady gains. Per pair of samples: a stereo strip (level, pan, accumulate) 35, two stereo strips in one fused pass 59, a centre strip 25, two centre strips fused 41, a send accumulate 19, a meter 15, round-and-saturate one channel 32 (MAIN with its centre accumulator, both sides 39), deinterleave 29, mono sum 13; a pair of samples is 32 pairs a block, plus about 50 a call. The HW-A load (image B: two sources hard-panned, no send, every meter on): input 1.0 k, drain 0.1 k, render **7.7 k** (two sine generators at about 45 cycles a sample, the float to Q15 at 15), strips 1.9 k (one fused pass), buses 2.2 k, meters 3.1 k (T01, T02, MAIN and MST pairs), convert 0.5 k: **about 16,500 cycles = 6.3 %**. For 12 live tracks, all centred, one send each (the shape of a new project; the voices and the effects are extra): strips 8.2 k (six fused centre passes), sends 7.9 k, buses 2.4 k, meters 8.7 k with all 17 live channels measured (1.0 k with only the master pair: `set_meter_mask`), input 1.4 k, convert 0.5 k, drain 0.1 k: **about 29 k = 11 %**, or **22 k = 8.3 %** with the master meters only; the same load before the review fixes modelled about 38 k (14.4 %: strips 13.8 k without the centre and two-track kernels, the scope and spectrum rings 1.9 k always written), against the ADR 0013 D1 table's 2 % for this row (that figure was a per-source add; the sends, the meters and the accumulator traffic are most of the difference). The pan law's two multiplies a sample are gone for a centred track, and a two-track pass saves the accumulators' load and store (11 cycles a pair of samples). Every route open on every track, every gain ramping, stems and REC on: about 95 k (36 %) [rough]. The model has been wrong by a factor before: the first version of this row counted instructions (20 a pair for a stereo strip), the disassembly says 24 instructions and 35 cycles | the stage columns, render max, the load that produced them |
| G-3 | B | Line out into a scope and a recorder. During the **level sweep** (script time 10-20 s after the tone started, repeated every 30 s): listen and look at both channels for 3 sweeps. | the amplitude of each tone follows the triangle (level 100 down to 20 and back: -20 dBFS down to about -52 dBFS, 0.4 dB a level step) smoothly: **no click, tick or step** at any point of the sweep or at its turns (a spectrum of the recording shows no broadband spikes; each tone's sidebands stay below -60 dBc), and the left tone stays on the left jack only, the right on the right | recording and spectrum |
| G-4 | B | The same during the **pan sweep** (script time 20-30 s). | track 1's 1 kHz starts on the left jack alone, **rises on the right jack to the same level as it moves to the middle** (this OS's pan law has no centre attenuation: at pan 50 both jacks carry it at full level), then **falls to nothing on the left** as it goes hard right; track 2's 1.5 kHz does the mirror image. Smooth, no click. At the end of the phase the script returns to the hard pans in one smoothed block | recording of both jacks |
| G-5 | B + a signal generator | **Input monitoring.** A sine or a square (about -20 dBFS) into the line input, a scope on the line output and the generator; `diag mon on` (level 70), later `diag mon 100`. **Never with a cable from OUT to IN** (feedback), and keep the headphones off the ears. | silence from the monitor until `mon` is typed (level 0 at boot); then the input on both jacks at the level of the fader (70 is -12 dB, 100 unity, 0.4 dB a step); **latency generator to jack** = the engine's two block slots, 128 frames (2.90 ms) + the codec's ADC and DAC delays (not found in the data sheet; est 1-2 ms): **expect about 4-5 ms**, and the same at every power-up (the HW-A soak varies `kTxSlots`); `mon off` returns to silence without a click. The measured figure is the first number the radar card "monitoring while recording" needs | scope capture of generator and output, ms |
| G-6 | B | `diag mix` while the monitor is on (G-5) with the generator at a known level. | IN L / IN R rows appear (a stereo source on one channel only shows one row sounding), MAIN and MST rows follow, the monitor level shows in the peak: -20 dBFS source at `mon 100` reads about -20 dBFS | the output |
| G-7 | A, B | `diag mem`. | SRAM_U in use about 64 KB (the engine is 13.8 KB; the taps left it) and free SRAM_U above 107 KB after the stack, over the 32 KB the budget wants; SRAM_L: **hot code about 14.6 KB of 24 KB** and **tap data 22,272 B of 24 KiB** on the `sram_l` line (a `budget tap_data 24 KiB` row follows); the SDRAM table and its sum unchanged | the output |

Not checkable on this hardware yet (no routing screen, no recorder, no voices): the cue bus and OUT MODE `MAIN L / CUE R`, REC into the arena, the send effects and the master chain's stages (all reserved or unarmed in this image), the stems. They are tested on the host and on QEMU; the first hardware look is HW-C.

## Hardware-session checklist: the display (OS-007c, for `hw-session`)

Images: **A** = the default build (HW-A's image A as well: the display runs the pattern view, static, and the audio path runs silent),
**DM** = `-DO3OS_DEMO_CYCLE=ON` (the cursor walks and the themes cycle), **A1 / DM1** = the same with `-DO3OS_DISPLAY_ROTATE=1` (the inverted
landscape). The names DM and DM1 are the display's own: HW-A's image C is the retune, not the demo. The skeleton images' rules above
apply unchanged (no update path, the recovery card, `fw-gate-check` and the maintainer's decision first); DM and the rotate variants only
after A has shown a picture. LED codes new with this slice: **4 flashes** = the FT812 did not come up (headless run; the heartbeat
follows), **5 flashes, once** = the display is up but the DMA self-test failed (polled path). No flashes of either = the display came
up with DMA proven. **8 or 9 flashes, repeated for good in place of the heartbeat** (the safe state, see "The display") = the display
stopped swapping frames and one re-init did not bring it back (8), or the app could not start (9): the screen is held dark.

| Id | Image | Steps | Expect | Capture |
|---|---|---|---|---|
| DSP-1 | A | Power up; film the screen from power-on at 60 fps or better. | The picture appears **before** the backlight (the screen is dark, then fades up over about 0.15 s, no garbage flash) showing the pattern view in the Amiga theme: 800x480, text sharp, no tearing, no stray lines. The heartbeat starts about 0.5 s after power-up. **Known remaining window:** after ACTIVE the FT812's backlight duty resets to full and the driver can only write 0 once the chip answers, up to about 60 ms later; if the backlight lights for a moment (a flash of white or of the bootloader's picture) in the first 0.2 s after PD#, that is the window (say how long): the bench decides whether the 60 ms post-ACTIVE wait can be replaced by earlier polling. | video of the screen and the LED |
| DSP-2 | A | Is the picture upright? | Upright (prediction: REG_ROTATE 0 is right on this unit). If it is upside down, repeat with image A1 and record both: it says which value this unit needs. | photo of each; the board revision and the panel's printed part number from the PCB |
| DSP-3 | A | Flash through the maintainer's flow (the bootloader hands over to the image), watch the **first** boot after flashing, then power-cycle and watch again. | The first boot after the bootloader is as clean as the power-cycled one (the OS-0.0.2 symptom was a corrupted first boot cured by a power cycle). If it is corrupt, say how (garbage, shifted, white, black) with a photo. | photo / video of both boots |
| DSP-4 | A | Scope or logic probe, if on hand: PD# (PTD11, Teensy pin 55), chip select (PTD15, pin 54) and SCK (PTD12, pin 53) from power-on. | PD# one low pulse of 20 ms about 0.1 s after power-up; chip select idle high between transactions; SCK silent until the init. If PD# does not reach the FT812 (not wired), the reset command carries the warm start: say so (ADR 0011 amendment 1). | capture |
| DSP-5 | A | Look at the LED right after power-up. | No 4 or 5 flashes. 5 = the byte-wide DMA write to `SPI2_PUSHR` did not check out: report it, the image still works by the slow path; 4 = no FT812 answer: check the SPI pins and PD# (DSP-4). | video |
| DSP-6 | A | Scope SCK during the init (the first 0.1 s after PD# rises) and during a frame; the F_BUS check is MK66-7. | init: 1.125 MHz; frame: bursts at 18 MHz, duty about 40/60 (22 ns narrowest), no ringing or runt pulses; each frame is one chip-select low period of about 1.4 ms for a 3.2 KB list. The margin to the FT812's 30 MHz limit is the point: if the picture is clean at 18 MHz, a later slice may try 20+. | screenshots of SCK and CS |
| DSP-7 | A | Film a stationary screen with a camera at 1/1000 s, or measure VSYNC / DE if a test point is reachable. | 54.1 Hz scan (18.48 ms a frame: 1056 x 525 clocks at 30 MHz); no flicker. | frame period |
| DSP-8 | DM | Watch 60 s of the demo cycle. | The cursor steps down a row every 1.5 s, the theme changes every 5th step in the order Amiga, Black, White, the cursor returns to the top every 24th; no tearing at the changes, no missed redraw; the glyph columns line up (the `VERTEX_TRANSLATE_X` on `VERTEX2II` question of OS-003b: a tile one pixel off is the finding). | video |
| DSP-9 | A | Compare the picture's colour depth and noise: `REG_OUTBITS` 0x1B6 (6 bits a line) with dither, and `REG_CSPREAD` 0 (the current setting) against 1 (Riedel's preset): the latter needs a one-line build change (`panel.h`). | smooth gradients (no banding) with the current values; a visible difference with CSPREAD 1 is worth a note (EMI is a later question). | photos |
| DSP-10 | A | At the lowest and a middle brightness, listen at the line out and the headphones with the display on. | No audible buzz from the backlight at its 250 Hz default (`REG_PWM_HZ` is not written). A buzz is PAR-066's symptom: the fix is a higher `backlight_hz` in `InitConfig` and it needs this note. | recording |
| DSP-11 | A | Ten minutes with the screen on; feel the display module and the regulators. | not hot (the FT812 is active, the backlight is on). | note |
| DSP-12 | DM | Leave it for 30 minutes. | No frozen picture, no corruption growing over time (a DMA or swap fault would show here); the LED heartbeat continues. | note / photo |

| DSP-13 | A | The boot logo (OS-003f), filmed at 60 fps or better from power-on: **the logo shows, sharp, centred, no garbage before it.** Look at the frames before the first light and during the fade. | The screen is dark, then fades up (about 0.15 s) **on the logo**, never on anything else: no flash of white, of the bootloader's picture, of the pattern view or of half a logo. The logo is a floppy disk with the Tracker on it, greyscale with a checkerboard dither, centred on the panel (a margin of 214 px each side and 54 above and below), with `Track3r  o3os <id>` in small type under it, the id equal to `diag version`'s. | video; photos of the first lit frame and of a still |
| DSP-14 | A | Time the logo: from the first light to the pattern view replacing it. | 0.8 s or a little more (the audio bring-up is about 0.6 s from the commit, so the first pass of the loop is near the minimum): never shorter than 0.8 s, never a second or more longer. The pattern view appears with no black frame between. | frame count of the video |
| DSP-15 | A | Look closely at the logo's pixels (a macro photo, or the video at full size). | Each logo pixel is a 3 x 3 block of the screen, sharp, no blur or colour fringe (the bitmap is sampled nearest). **[A] one column of blocks is 4 px wide (about x = 340, the 43rd) and the last is 2 px, and likewise one row of each:** say where they are (the chip's rounding of 85/256; the simulator puts them at screen column 126 and 370 of the logo). If the chip draws the logo with soft edges, or the dither as noise, or garbage, that is the finding. | macro photo |
| DSP-16 | A | Power-cycle ten times, and once with a key held (Shift + F1 + F8: the INPUT TEST boot chord). | The logo every time, in the same place; the chord still opens INPUT TEST after it. | note |

Not checkable yet, recorded for later slices: DMA next to the audio engine's DMA (priority and bus contention, HW-A); `REG_FRAMES` and
the bring-up counters (`last_bring_up()`) read out by `diag`; the CMD_SNAPSHOT2 golden calibration of the simulator (the
coprocessor command path `Chip::cmd_write` / `cmd_wait_idle` / `cmd_check` is in place and host-tested, the USB side that carries a
snapshot is not); fault injection on the coprocessor (a deliberately bad CMD_INFLATE) for the recovery path; the **swap-stall recovery** (code 8, the
re-init and the bitmaps' re-upload) needs a unit whose scan-out can be stopped on purpose (for example PCLK written to 0 from a
debug build): host-tested against a chip model only.

## Hardware-session checklist: input (OS-007d, for `hw-session`)

Images: **A** = the default build (the boot chord opens INPUT TEST), **I** = `-DO3OS_INPUT_TEST=ON` (starts on INPUT TEST; for everything below except INP-8), **IA** = I with `-DO3OS_AUDIO_TEST=ON` (INP-11).
The skeleton images' rules apply unchanged (no update path, the recovery card, `fw-gate-check` and the maintainer's decision first). Read the screen as described in "The INPUT TEST screen".

| Id | Image | Steps | Expect | Capture |
|---|---|---|---|---|
| INP-1 | I | Power up cold; look at the LED for 15 s and at the strip: `PADS OK  KEYS OK  ERR 0`. | no 13 flashes; both scanners answer; after 10 minutes of idle `ERR`, `DROP`, `RSYNC` still 0. A 13 means a scanner did not answer: note which (`PADS NO` / `KEYS NO`), then the scope item INP-12. | photo of the screen, the LED |
| INP-2 | I | Press each of the 33 buttons once, firmly, and let go; then once each very fast (a flick) and once each as slowly as you can. | each button's count is **1** per touch ("every control registers once per press, no bounce"); `FLT` may rise (that is the filter working); a count of 2 for one touch is a bounce the filter missed: say which button and how it was pressed. | photo of the screen after the round |
| INP-3 | I | The same for the 48 pads, with a light touch, a hard hit and a slide of a finger across a row. | each pad's count is 1 per touch; a slide lights the pads in order, none twice. | photo |
| INP-4 | I | Button and pad **mapping**: press every button and read its cell against the label on the panel; press the pads at the four corners and the middle of the top row. | each button lights its own name (33 of 33); **pad 00 is the top left, 11 the top right, 36 the bottom left, 47 the bottom right** (inferred): report any different order as a table pad -> position. | photo, the table |
| INP-5 | I | **Encoder direction and detents**: clear (F2 + F3), turn one click clockwise, then one anticlockwise, then ten clicks quickly. | one click = `TOTAL +1` (clockwise positive), `LAST +1`; the click back = `-1`; ten quick clicks = `+10`. A full turn is 24 clicks. If the sign is reversed, flip `kEncoderCountUpIsClockwise` (`input_map.h`). If a click sometimes reads 0 or 2, say how often and in which direction (detent phase). | video |
| INP-6 | I | **Fast spin**: clear, spin the dial as fast as you can for about 3 turns, stop; then 10 turns slowly. | `TOTAL` is +-72 for 3 turns and +-240 for 10 (24 a turn), none lost at any speed (the counter is hardware). `DROP` stays 0. Note the largest `LAST` you saw. | photo, count of turns |
| INP-7 | I | **LEDs**: hold a pad, then several at once; watch the others; film the pads in a dim room. Then, on the Pattern screen, change the theme (Shift + Master, three times round) and hold a pad in each theme. | on INPUT TEST a pad that is down lights at the **front** level and the others at the **back** level (PWM about 183 and 20 in Amiga); on every other screen only a held pad lights; no flicker, no flash at power-up before the screen appears (also after a reset of the MCU alone, for example the fault-drill image's, when the chip still holds the last frame; a unit whose pads never light and whose `PadLeds::errors()` rises would be the chip refusing writes in software shutdown); the three themes differ a little in level; no pad lit that is not held; nothing warm after 10 minutes with 10+ pads held (touch the board near the LED driver). Rate the front and back brightness against the official firmware's on the same unit (if the maintainer has its image) and say whether the back level is visible enough. | video, a note on the brightness |
| INP-8 | A | **Boot chord** (needs the maintainer's hands at power-on): power off, hold Shift + F1 + F8, power on, keep holding until the screen shows INPUT TEST (the logo is up about a second first). Then repeat with no key held, and with only F1 + F8 held. After EACH power-on, over the console: `log 1024` and look for the two `boot: chord` lines (they come before the sample loading, which pushes them out of the last 48 lines). | the chord opens INPUT TEST with Shift, F1 and F8 shown down and counted 1; no chord and a partial chord open the Pattern screen. (This also proves a key held at power-up reaches the TCA8418's FIFO.) OS-DIAG1: the log has, in this order, `boot: chord keys held (bit 0 Shift, 1 F1, 2 F8) 0x00000007` (0x7 = all three; the first line is the keys at the moment the poll ended) and `boot: chord scan N steps, M ms, buttons down 0x........, bus errors E, online O` (N = 1 ms passes before the chord was complete, 150 = timed out; `buttons down` = every button the scanner holds down, bit 29 Shift, bit 0 F1, bit 7 F8, so 0x20000081 is the chord; `online` 3 = both chips answered). If the screen is NOT INPUT TEST: held 0x7 and `boot: display re-initialised, the app starts over` = the app was rebuilt (OS-DIAG1 sends it back to INPUT TEST, so the screen should still be right); held 0x7 and no such line = a bug after the read (report it); held < 7 with `buttons down` 0 and E 0 = the chip reported no press for the held keys (then the next fix is to read the matrix as GPIO, not from the FIFO); held < 7 with some buttons down = a partial chord (keys released early, or one key not reaching the chip); bus errors > 0 or online < 3 = a scanner chip did not answer at boot (LED code 13). | video, the two log lines per power-on |
| INP-9 | I | **Chords**: press F4 alone, F5 alone (they count, nothing happens), then F2 + F3 together, then F4 + F5 together. | F2 + F3 clears every count and the dial; F4 + F5 leaves for the Pattern screen. Back on the Pattern screen the buttons work: Song, Pattern, the arrows, F8 = UNDO says "nothing to undo", the encoder scrolls the pattern. | video |
| INP-10 | I | **Latency**: film the pads and the screen together at 240 fps; hit a pad, count the frames to its blue cell. Read `AGE` and `MAX` on the bottom strip over a minute of play. | the picture follows within about two screen frames (the app ticks every 16 ms, the display scans at 54 Hz: 20-40 ms is expected); `AGE` (the time from the scan to the app taking the event) is a few ms, `MAX` below about 20 ms. State the numbers: they decide whether the scan needs an interrupt. | video, the numbers |
| INP-11 | IA | **Shared bus**: with the tone running (HW-A F) press keys for ten minutes, slide across the pads, spin the dial; watch `ERR`, `RSYNC` and the audio. | no audible click or dropout when a key is pressed or the LEDs change (the codec and the buttons share I2C0); `ERR` stays 0; the HW-A counters stay clean. An `ERR` that rises while the tone plays is the first finding of the shared driver. | recording, photo |
| INP-12 | I | If a scope or logic probe is on hand: SCL and SDA of I2C0 (PTD8 / PTD9) and of I2C2 (PTA12 / PTA13), and the INT lines PTC11 / PTD1, during a key press. | SCL 375 kHz (+-1 %) with clean edges on both buses and no line stuck low; INT falls when a key goes down and rises after the read. If INT does not move, say so: the safety poll covers it (`AGE` then reads up to about 10 ms after a press). | screenshots |
| INP-13 | I | Switch the unit off and on ten times in a row, then once with a cable-level wiggle of the front panel's flex connectors (only if the maintainer agrees). | the screen reads `PADS OK  KEYS OK` after every power-up; no 13; a chip that drops out during the wiggle comes back within a second (`ERR` counts it, `RSYNC` counts a lost chip) and no key stays down. | note |

Not checkable yet, recorded for later slices: the TCA8418's own debounce time (the lockout assumes it is short), the encoder's mechanical detent phase against the counter at rest (INP-5 shows it only
as a symptom), the IS31FL3731's current at full brightness on all 48 pads, the power switch, a scope measurement of the scan pass's duration (a GPIO marker pin once one is free).

## Hardware-session checklist: USB and `diag` (OS-006b, for `hw-session`)

Image: the **default build** (it enumerates; the pull-up goes on after the audio path is up). The skeleton images' rules apply unchanged (no update path, the recovery card, `fw-gate-check` and the maintainer's
decision first). Use a powered hub and a laptop port, and a terminal on each host OS. Read "USB and the `diag` console" first. **Nothing in this driver's register writes has been run on a unit.**

| Id | Steps | Expect | Capture |
|---|---|---|---|
| USB-1 | Plug the unit into a Linux host; `lsusb -v -d 16d0:0e9f`, `dmesg`. | enumerates first time; "Track3r", serial 16 hex digits (the same after a power cycle, different on another unit); one configuration, four interfaces; a CDC ACM port `/dev/ttyACM*` and an ALSA MIDI port; no errors in `dmesg` (no "device descriptor read/64, error", no "cannot enable"). | `lsusb -v`, `dmesg` |
| USB-2 | The same on macOS (System Information, `ls /dev/cu.usbmodem*`, Audio MIDI Setup) and on Windows 10/11 (Device Manager: COM port and MIDI device, no yellow mark). | the same: a serial port and a MIDI port named Track3r; Windows needs no driver for either. | screenshots |
| USB-3 | **Clock and the MPU grant:** with `diag usb` open, enumerate and run a minute. | `bus errors` all 0, in particular **DMA 0** (nonzero DMA means the MPU grant of master 4 was wrong or the bus master cannot reach the buffers), no `stray tokens`, no `ping-pong resyncs`, no `loop limit hit`. If the host cannot enumerate at all: note `usb` `bus errors` (PID / CRC16 / timeout point at the clock). | `diag usb` text |
| USB-4 | Every command: `help`, `version`, `stage`, `cpu`, `mem`, `crash`, `usb`, `log`, `log 5`, `input` (turn the dial, press pads, then any key), `input stats`, `reset`. | each answers within a second, text complete (nothing cut: the `mem` and `usb` replies are 0.9 and 1.2 KB); `cpu` and `mem` match the figures of the HW-A checklist; `log` shows `boot:` and `usb:` lines in order with sane millisecond stamps; `input` shows each press once; `reset` reboots the unit and the port comes back (a terminal sees the unit disappear and return). | terminal transcript |
| USB-5 | Editing: type `help` with mistakes and backspace, arrow keys, Ctrl-U, Ctrl-C, a line of 100 characters, paste five commands at once, then three `log 48` in one go. | the line editor behaves as described (arrows never reach a command; an overlong line is refused loudly, never run truncated); pasted commands all run and every reply is complete (no line missing from any of the three `log 48`); `usb` shows `dropped-chunks 0` on both sides and `holds` only if the paste was long. | transcript |
| USB-6 | **Unplug and replug** the cable ten times, once during `input` streaming, once during `log`. | the host re-enumerates every time; `diag usb` shows one more bus reset and a suspend each time; the unit never needs a power cycle; the terminal reopens and the banner appears when DTR rises. | `diag usb` after, `dmesg` |
| USB-7 | **Suspend and resume:** put the laptop to sleep with the unit attached (or `echo auto > power/control` and `echo suspend`), wake it. | `diag log` shows `usb: suspend`, then `usb: resume`; the port works after the resume; the unit stays up (no transceiver power-down). Note the current draw if a USB meter is at hand: the unit does not meet the 2.5 mA suspend limit by design. | `diag log`, meter |
| USB-8 | **Hub and no host:** a bus-powered hub, a self-powered hub, a charger with no data lines, and the unit powered with no cable at all. | enumerates through both hubs; with a charger or no cable the unit runs normally (the pull-up with no host listening is harmless: watch for any warmth or reset). Measure D+ with a scope with no host if one is at hand: pulled up to 3.3 V through 1.5 k. | note |
| USB-9 | **CDC throughput:** `cat` a long reply loop: `while true; do echo mem > /dev/ttyACM0; done` with a reader (or a script that sends `mem` 1,000 times and counts complete replies). | every reply complete; `usb` shows `dropped-chunks 0`; state the replies per second. Then stop reading (`stty` stop, or a terminal paused) for 10 s with the loop still sending: the unit stays alive, the shell stops taking input (the transmit ring has no room), the host's writes are NAKed (`holds` rises, `dropped-chunks` stays 0), and after the host reads again every reply that was asked for arrives. | counts |
| USB-10 | **No audio underruns while `diag` streams:** with the HW-A tone running (image IA of the audio checklist: `-DO3OS_AUDIO_TEST=ON`), run `input` while pressing keys, then the throughput loop of USB-9, for ten minutes. | the HW-A counters stay clean (`diag cpu`: underruns 0, late 0, slips 0, jitter as before), no click in the tone; the LED stays on the heartbeat. | `diag cpu` before and after |
| USB-11 | **Interrupt cost:** read `diag usb` "isr: ... worst N cycles" after USB-4, USB-9 and USB-10. | state N: the estimate is a few thousand cycles worst (about 17 us); a figure above 20,000 cycles (110 us) is a finding to report with the situation. | the lines |
| USB-12 | **IRC48M accuracy:** with a counter or a good scope on a host that sends exact frames, or from `diag usb` bus errors after 10 minutes of USB-9. | no CRC / PID / timeout errors after the first second (clock recovery locks within a few frames). | `diag usb` |
| USB-13 | **Windows driver cache:** enumerate on Windows, then (when AUDIO mode exists) switch mode. Not testable with this slice (no AUDIO), recorded so that the `bcdDevice` split of ADR 0012 amendment 3 is checked in the audio slice. | n/a | |
| USB-14 | `python3 -I tools/o3console.py --port /dev/ttyACM0 version stage usb "log 30"` (and `--list`). | each reply complete and cleaned (no CRs, no echo, no prompt), exit status 0; with the unit unplugged or silent, status 2 after the timeout, never a hang; `reset` is passed through and exits 0. | the output |

Not checkable yet: a fault inside the USB interrupt (the crash record would carry module `0xD3` only during `usb0::start()`, not during the interrupt); the board's VBUS and ID routing (ADR 0012 validation item 1);
the bus current (`bMaxPower` is the 100 mA placeholder).

## Hardware-session checklist: the SD card (OS-007e, for `hw-session`)

Image: the **default build**. The skeleton images' rules apply unchanged (no update path, the recovery card, `fw-gate-check` and the maintainer's decision first). Read "SD card" first. (OS-006c adds a writer, so for a build with `diag fw` the tab below proves nothing unless the slot's write-protect contact is wired and read: do not run `fw put` in these rows.) **Nothing in this driver's register writes
has been run on a unit.** The card is the maintainer's: **work on a copy of the card's files on a second card for the first rows (SD-1 to SD-5), then the maintainer's card; set the card's write-protect tab for every row (the
driver never writes, the tab is a hardware backstop that proves it)**. Capture with `python3 -I tools/o3console.py --port /dev/ttyACM0 sd "log 48" "ls /" ...`.

| Id | Steps | Expect | Capture |
|---|---|---|---|
| SD-1 | Power on with **no card**. `diag sd`, `diag log`. Then insert a card and `diag sd` again; remove it; `diag sd`. | boot reaches the heartbeat, no hang: `boot: sd NOT mounted: no card`, no fault in `diag stage` (an empty slot is not one); `sd` says "no card (detect switch open)", then "a card is in" (the PTD0 level follows the card, active low), then open again. | `diag sd`, `diag log` |
| SD-2 | Power on **with a copy of the maintainer's card** (FAT32, 14.8 GB). `diag log`, `diag sd`. | `boot: sd mounted, FAT32, <MB>`: the MB figure is the volume's data area in MiB (about 14,100 for a "14.8 GB" card), cluster size as formatted; the mount took tens of ms (the log's stamps). | log lines with ms stamps |
| SD-3 | **Identity**: compare `diag sd`'s card line and `id:` line with the card's label and with `cat /sys/block/mmcblk0/device/{manfid,name,serial,date}` (or `udevadm info`) on a PC. | manufacturer, product name, revision, serial and date match; capacity MB close to the label. `diag sd` also prints the raw `cid:` and `csd:` registers as 32 hex digits: they must equal `cat /sys/block/mmcblk0/device/cid` and `csd` on the PC except for the last byte (the CRC7, which the controller does not hand over: ours is 00). A line that is garbage or shifted by a byte means the R2 response layout (`unpack_r2`, "(RM)") is wrong: report both. | `diag sd`, PC output |
| SD-4 | **Speed**: `diag sd`'s bus line. Read a large folder (`diag load Projects/<biggest>`), then `diag sd` counters. | "4 data lines, 45000 kHz (high speed)"; `retries`, `failed reads`, `data` and `data-timeout` all 0 after several loads. Any CRC errors at 45 MHz: rebuild with `-DO3OS_SD_HIGH_SPEED=OFF` (22.5 MHz) and repeat: say which. | `diag sd` before and after |
| SD-5 | `ls /`, `ls Projects`, `ls Workspace`, `ls Workspace/patterns`, `ls "Projects/<a name with spaces>"` against a PC listing of the same copy. | the same names (long names, spaces, mixed case, UTF-8), sizes and directories, the same count; no `.`/`..`; `ls` of a file or a missing name says so. A key press stops a long listing. | `ls` output, PC `ls -l` |
| SD-6 | **The boot load**: a card whose `Workspace/` was left by the OG (a project, patterns, instruments). Power on; `diag log`; look at the screen. | `boot: loading Workspace`, `boot: project loaded`, `Workspace: v<N>, <P> patterns, <I> instruments` with P and I equal to a PC count of `pattern_NN.mtp` and instruments (a `.pti` beside an `.mti` counts once), `load: bytes read` close to the sum of those files' sizes (+ the instrument headers only), the screen shows the Pattern view of the loaded project (name, tempo and pattern 1 as the project files hold them) a moment after the logo. Time from `boot: loading` to `boot: project loaded` from the stamps: state it (expected tens to a few hundred ms). | log, screenshot, the PC count |
| SD-7 | `diag load Projects/<name>` on each of the maintainer's projects (versions 2 to 16 are in the OG fixtures; 1.9.2 writes 17). | each says `project loaded` with its version (`unverified version` only for layouts nobody has seen); a deliberately damaged copy (flip a byte of a pattern file) loads with `1 failed`; the screen holds its last picture during the load and then shows the new project. State the read speed: `load: bytes read` over the log's ms. | log lines |
| SD-8 | **Audio unaffected**: with the HW-A tone running (`-DO3OS_AUDIO_TEST=ON`), `diag load` a big project and `ls /` repeatedly; `diag cpu` before and after. | no click, `underruns 0, late 0, slips 0`, the same jitter; the render maximum unchanged within the noise (the card reads happen in the main loop; the audio interrupts preempt them). | `diag cpu` before and after, a recording |
| SD-9 | **Read-only, on the card**: with the write-protect tab **off** on a copy, run SD-2 to SD-7, then compare the copy with the original (`diff -r`, and `fsck.vfat -n`): nothing differs, the volume's dirty bit is not set; then repeat with the tab on. | the copy is byte-identical afterwards (no new file, no changed time stamp, no `.Trash`, nothing); the tab changes nothing the firmware does. | `diff -r` and `fsck.vfat -n` output |
| SD-10 | **Card pulled out**: during `diag load`, during `ls` of a big folder, and while idle; then put it back and `diag sd mount`. | the OS keeps running (heartbeat, `diag` answers, audio unaffected); the load ends `FAILED i/o error` or `no card`, `ls` says `not mounted` or an error; `sd` says no card; after reinserting, `diag sd mount` mounts again and `ls` works. No bus fault, no crash record. | `diag log`, `diag sd` |
| SD-11 | **Other cards**: a second FAT32 card of another make, an old standard-capacity (SDSC, up to 2 GB) card if there is one, a 64 GB exFAT card. `diag sd`, `ls /`. | each mounts and lists; `sd` names SDSC / SDHC / SDXC and the file system; the SDSC card is byte addressed (it works); exFAT lists and reads. A card that does not mount says why (`unsupported`, `i/o error`) and the OS runs on. | `diag sd` per card |
| SD-12 | **Worst cases**: a card with no file system (a freshly erased one), an ext4 card, a card whose FAT is corrupted (a fsck-damaged copy), a `Workspace/` with a 12,816-byte `patternsMetadata` and 255 pattern files. | `not mounted: unsupported` for the first two (a card is in: `diag stage` lists the SD fault), no hang; the corrupted copy gives errors or a partial load, never a hang or a crash; 255 patterns load (the listing is cut only at 4,096 entries, which no OG folder reaches). | `diag log`, `diag stage` |
| SD-13 | **The samples load** (OS-007f): a copy of the card whose `Workspace/` is the maintainer's 1.9 project (instruments as `.pti` with `samples/instrNN.wav` beside them). Power on; `diag log` with its ms stamps; `diag sd`; watch the screen while it loads. | `boot: project loaded`, then `boot: samples N loaded, M missing, K KB` with N the number of instruments, M 0 and K the sum of the PCM sizes in KiB of a PC count (`.pti` size - 392, divided by 1024); `diag sd` repeats it on a `pcm:` line; the screen keeps its picture and the backlight stays lit through the load (the display is serviced every pass), the heartbeat runs, `diag` answers. Note the time from `boot: loading Workspace` to the samples line and K: that is the card's sample read speed (the 16-instrument fixture is about 7.8 MB), the number that says whether ADMA2 (SD-7) is wanted. Then Play: the pattern plays the project's own sounds, not the demo kit. | the log lines with stamps, `diag sd`, K and the seconds |
| SD-14 | **It sounds as the OG's does**: the same project on an official 1.9 unit and on this one, pattern by pattern: each instrument's pitch (tune and fine tune: try an instrument with +/-24 and +/-100), its window (start and end), a forward-loop instrument sustaining (play mode 1), its volume against the others. Record both outputs if possible. | the same notes at the same pitch (this OS plays a file's PCM at its engine rate, 44,117.65 Hz, and applies no 44100 / 44117.65 correction, 0.7 cent; whether the official firmware's clock differs from 44,100 Hz is an open research question, docs/hw/tracker-hardware-notes.md section 5), the same windows, loops sustain, levels in the same order. Known differences, not failures: pan, sends, filter, envelopes and LFOs are not applied; backward, ping-pong and granular instruments play as a one shot (`load: instruments played one shot` says how many); the slice FX and the wavetable position envelope are not applied. Slice, beat slice and wavetable instruments are bound as OS-007h models them: `diag inst <n>` shows what instrument n got (mode, window start and length, loop, root, rate, volume, a 16-bit sum of its first 64 samples to set against the file); velocity is linear (V1). | notes on each difference, the recordings |
| SD-15 | **Failure paths, on a copy of the card**: (a) delete one `samples/instrNN.wav` and, for another slot, its `.pti`'s sibling WAV and the `.pti` too; (b) a 48 kHz WAV in a slot; (c) a project larger than the arena (10,816 KiB; several copies of the biggest samples in more slots); (d) `diag load Workspace` while a pattern is playing; (e) pull the card during the sample load; (f) a Workspace with no instruments. | (a) `load: instrument NN sample not found` for those slots only, the others play, the missing ones are silent, the load ends; (b) `unusable` (this OS takes 44,100 Hz only in an OG folder; whether the official firmware refuses other rates is an open research question); (c) `load: sample arena full, instruments silent N` and what fits plays; (d) the music stops at once without a click storm, the badge says STOP, the screen holds its picture until the end, Play works afterwards and plays the new project; (e) the load ends with the card's error in the samples line (`i/o error` or `no card`), what was bound plays, the OS keeps running; (f) `boot: the project has no sample instrument: demo kit` and Play plays the demo. Never a hang, a crash record or noise from a half-written sample. | `diag log` for each, `diag sd`, `diag stage` |
| SD-16 | **OG export, part one** (OS-FILES-C C2a): on a card (any SD card, no marker), with a project loaded (`diag load Projects/<name>` of an OG project, or the demo), transport stopped: `diag ogexp`, then `diag ogexp status` until it says DONE; `diag ls "Export/<name> OG"` and `diag ls "Export/<name> OG/patterns"`; `diag load "Export/<name> OG"` and `diag inst`/play a pattern; `diag ogexp` again (OG 2); `diag ogexp` while a pad take is recording and with the transport playing; `diag ogexp cancel` after the first file; pull the card during an export, put it back, `diag sd mount`, `diag ogexp` again. Also on a write-protected card. | DONE with n/n files; `project.mt`, `patterns/pattern_NN.mtp` (NN two digits at least) and `patterns/patternsMetadata` are there and no `_o3save.tmp`; the second export is `OG 2`; the load reads the same project back (patterns, tempo, song; no instruments: C2b); a take or the transport refuses with its reason and writes nothing; cancel and a pulled card leave whole files only, no `project.mt`, and the OS never removes the folder; write-protected card: `CARD IS WRITE PROTECTED`, nothing written. `diag cpu` before, during and after: no underruns, over-budget 0 (the export runs on the main loop, cost and MIDI-in jitter to be measured, T16). **An official unit opening the folder is NOT checked here** (T9, the maintainer). | `diag log`, `diag cpu`, `diag ls` |

Not checkable yet, recorded for later slices: a card-detect interrupt and hot-plug; the power draw of the slot; a measured SDHC interrupt-free read speed against ADMA2 (SD-7's figure is the number that decides whether the sample slice needs it).

## Hardware-session checklist: the firmware update path (OS-006c, for `hw-session`)

Image: the **default build**, first installed **by card** as every image so far (`fw-gate-check`, the recovery card with exactly one known-good `.ptf`, the maintainer's decision). Everything below is on the maintainer's **test card** (volume XXXX-XXXX, with a
`Firmware/` folder that holds the known-good image); keep that image as the first `.ptf` in directory order (`fw` shows it with `*`) so the reset button always flashes it. **Nothing of this has run on a unit**; the SDHC write registers and PTA1 are the parts that only the unit can show.

| Id | Steps | Expect | Capture |
|---|---|---|---|
| FW-1 | Install the OS-006c image by card as usual. `o3console.py ... fw`. | `fw: write path ENABLED`, `volume XXXX-XXXX, FAT32, Firmware/ yes, write-protect no`; the listing equals `ls Firmware` (same names and sizes, same order) with `*` at the first `.ptf`; `upload: idle`. | `fw`, `ls Firmware` |
| FW-2 | A card with a `Firmware/` folder and no marker file of any kind (OS-FWANY, OS-ANYCARD): `fw`, then `o3console.py --put <a known-good image> --name o3os-0.9.ptf`, then a corrupt `.ptf` (flash-configuration byte damaged; scratch copy of `o3console.py` with `local_check` neutered), then `fw rm o3os-0.9.ptf`. Then one with no `Firmware/` folder, then one formatted exFAT, each with `fw put` of a good file. | the card: `write path ENABLED`, the good put `ok ...`, the corrupt one `ERR ... at <offset>` with no temporary file left, `fw rm` `removed`, `fsck.vfat -n` clean; the folderless card `no Firmware/ folder`, exFAT `unsupported`, each `fw put` one `ERR` line and **the card unchanged (`diff -r`, `fsck.vfat -n`)**. `--put` needs no `--expect-volume` (optional check). | outputs, `diff -r` |
| FW-3 | Does the slot's write-protect contact do anything? Set the card's tab (if it has one), `fw`, then `fw put`. | `write-protect no` is expected (the contact is not read): **record whether the put is then accepted**: it is, unless the slot or the card refuses writes; this decides whether the tab can be a backstop at all. A card whose CSD is write protected (a locked card) says `write-protect yes` and every write answers `ERR write protected`. | `fw`, the put's reply |
| FW-4 | **The SDHC write phase.** `o3console.py --put <a known-good image, e.g. the one on the card> --name o3os-0.9.ptf --expect-volume XXXX-XXXX`. Then on a PC: `sha256sum` of the file on the card against the original, `fsck.vfat -n`, `ls -l --sort=none`. | `ok o3os-0.9.ptf <bytes> <crc> image <len>` and `first-ptf <the known-good name>`; the file on the card is byte-identical to the one sent; no `_o3up.tmp`; `fsck` clean; the directory order shows the new file after the old ones. Note the time of the put (the log's stamps) and `diag sd`'s write counters (`blocks written`, `failed writes` 0). A CRC-status error, a data timeout or a busy that never ends here is a register-policy bug: report `diag sd` and `diag log`. | the reply, `sha256sum`, `fsck`, `diag sd`, `diag log` |
| FW-5 | **Audio and display unaffected.** With the HW-A tone running (`-DO3OS_AUDIO_TEST=ON` build) or any playing pattern, repeat FW-4 and `diag cpu` before and after. | no click, `underruns 0, late 0, slips 0`, the same jitter, the screen keeps drawing, the heartbeat runs. | `diag cpu` before and after, a recording |
| FW-6 | **Refusals and errors.** (a) a file with a damaged flash-configuration byte or one flipped byte in the middle, sent with a scratch copy of `o3console.py` whose `local_check` is neutered (the real tool refuses it before sending anything); (b) a put of an existing name; (c) a put to an official name (`PolyendTracker_1.9.0.ptf`); (d) `--expect-volume 0000-0000`; (e) a put of a file larger than the free space (a nearly full card). | (a) `ERR ... at <offset>` (`flash configuration field is not the required one` for the field), the temporary file gone, the prompt back within 5 s; (b), (c) `ERR already exists`; (d) the tool refuses before sending anything; (e) `ERR no space` before `ready`. No `.ptf` appears in any case, and the first `.ptf` in `fw` is still the known-good one. | each reply, `fw`, `ls Firmware` |
| FW-7 | **Pulled cable mid-put**: start a put of a large image and unplug USB half way; plug it in again, `fw`, `ls Firmware`. Then **a card pulled mid-put** (a copy of the card), put it back, `sd mount`, `fw`. | the unit survives; `Firmware/_o3up.tmp` is gone after the cable case (the abandoned upload is deleted) or replaced by the next put (the card case); no `.ptf` of the cut upload; `fw` works after `sd mount`; no crash record. | `fw`, `ls Firmware`, `diag log` |
| FW-8 | **Arm and update, to the known-good image** (a different name than the one the unit runs, e.g. `o3os-0.9.ptf` if it is the same build, or a rebuild with a different `O3OS_BUILD_ID`): `o3console.py --update o3os-0.9.ptf --crc <crc> --yes-update --expect-volume XXXX-XXXX --expect-version <id>`. Watch the unit, the LED and the port. | `armed <token>`, `update: handing over to the bootloader (...)`, the unit leaves (screen/LED/port), the bootloader flashes, the unit comes back, `version` shows the expected id. `Firmware/_fwinfo` is **absent** afterwards (the bootloader deleted it). How long each step took; whether the screen shows the bootloader's own picture. **If it does not leave within 5 s** the tool reports `ERR bootloader did not take over`, `_fwinfo` is gone and the pin is released: say what the unit showed (this decides whether the pin alone is enough). | the tool's output, photos/video, `ls Firmware` |
| FW-9 | **Reset-button ordering**: after FW-8 the card holds several `.ptf`. `fw` shows which is first. Press the hidden reset button (with the known-good first): the known-good image is flashed. | the button flashes the file `*` marks. If another one is flashed, the directory order the bootloader walks is not FatFs's: record it. | `fw`, the version after the reset |
| FW-10 | The 5 s fallback cannot be provoked safely (it needs a bootloader that does not react) and **stays unverified**: record whether the pin's resting state after a release (the restored reset default, pulled up) matters; if the maintainer can scope PTA1 during FW-8, record its level before, during the 5 s and after the reboot. | notes only. | scope capture if available |

Not covered: `fw` on a card that other software writes at the same time, the long-term wear of the card.

## Follow-ups (not in this slice)

- OS-007e (the SD card): the peak cache of the loaded samples; reloading one instrument's sample after an edit; `patterns/patternsMetadata` (names); a `--card <dir>` option of `o3sim` on `DirStorage`; ADMA2 behind `SdBus` if SD-7's
  speed asks for it; card-detect debounce and a hot-plug event instead of `sd mount`; a "LOADING" notification in the app instead of a held frame; FatFs R0.16 when the maintainer wants the upstream's newest (a drop-in, see THIRD_PARTY.md);
  extending `check_layers.py`'s no-heap rule to `platform/mk66` (the loader is in `app`, which it covers).

- Stack guard through the SYSMPU and a painted stack with a high-water mark (see "No stack guard").
- The watchdog's deliberate re-enable (see above), after a unit has shown a reset and its crash record.
- The LMEM code cache and the SDRAM code-bus alias (ADR 0002 / 0013 HW-D).
- `check_layers.py` enforces no `double`, no heap and no `new` only in core..app; extend it to `platform/mk66`.
- The display: a persistent orientation setting and the boot-time flip chord (need the settings store; the input exists since OS-007d);
  `diag` counters from `DisplayStats` and `last_bring_up()`; the CMD_SNAPSHOT2 path for the simulator calibration; a runtime brightness setting
  on top of `Ft812Display::kBacklightDefault`.
- OS-006b: AUDIO mode's isochronous endpoints and the stem/return streams (the descriptors, packetiser and feedback exist since OS-006a); the engine's binding of the two MIDI rings (OS-005); persistence of the crash record beyond the run after the crash; `usb0` registers behind a host-visible fake would need a register-level model (QEMU has no USB0).
- A QEMU test of the fault stub's register choreography (the naked stub is reviewed against its disassembly and proven on
  hardware by MK66-8, not on QEMU).
- OS-005a: the I2C codec link on an interrupt or DMA instead of polling only if
  something other than start-up and the unmute ever needs it; a GPIO marker pin for the render (scope-measurable cost and jitter
  without a probe) once a free pin is known.

## Open questions for the maintainer

(OS-007d, after the list below: 8 to 11.)

1. Is the status LED (`kStatusLed`, PTC19, Teensy pin 78) a visible LED on your unit, or only a pad? (MK66-1/-2 settle it; if neither, the
   first session needs a scope.)
2. The 16 MHz crystal and the SDRAM part are assumed to be the OG's (the same board design). Do you have any evidence of a
   different revision (display orientation already differs between units, `docs/hw/tracker-hardware-notes.md` section 1)?
3. `_fwinfo`: does the bootloader read a version from the image? (See above; still open after OS-006c, which writes only the bare name.)
4. May the next slice enable the watchdog once MK66-8 has shown a reset and its record on a unit?
5. (OS-005a) How will HW-A read the numbers (`diag cpu`) before the CDC console exists: does the Tracker expose a debug port, or does
   HW-A wait for OS-006b? Without either, the LED verdict and the tone are all it can show.
6. (OS-005a) The headphone level of the test tone: code 0x40 (-20 dB) with the tone at -20 dBFS. Quieter or louder for the first
   listen? (`kHpUnmuteCode` in `sgtl5000.h`; never louder than code 0x30, a tested limit.)
7. (OS-005a) `CHIP_I2S_CTRL` 0x0130 says SCLK = 32 Fs (as PJRC's Audio library writes it) while the K66 drives 64 Fs, and the data sheet says a
   slave's field must match the SCLK it receives (0x0030). The default stays the proven value; HW-A row E-1 is the A/B. No decision needed
   until its result.
8. (OS-007d) **Encoder acceleration.** A burst of turns sent at once is multiplied on a unit running this OS (observed with `o3hil.py`: asked 40 detents, got 100). That is this OS's behaviour (what the official firmware does with a burst is an open research question): the HAL here reports plain detents and has no table, so the multiplication happens above it (where is an open research question). Should acceleration be a policy of the app
   (a per-binding or a per-parameter setting, which fits the gesture layer), or should the HAL apply a table?
9. (OS-007d) **The power switch (PTC3).** *Answered by OS-POWER (2026-10-17): a platform input outside the binding chain (a hold of 2 s switches the unit off; `docs/specs/OS-POWER.md` D1), see "Power switch (OS-POWER)" below.* It is not an `app::Button`; Shift + Power is reserved for a later screenshot item (`OS-SCREENSHOT`).
10. (OS-007d) **The pad numbering and the LED level.** Pad 0 as the top left is inferred from the note layout of the pads (INP-4 settles it). Should the OS keep two fixed levels (front 255, back about 69 PWM) as the
    default, or is the palette to be tuned on the unit (INP-7)? There is no total-current cap on the LEDs: want one?
11. (OS-007d) **LED code 13** (a key scanner did not answer at boot) is played once and the OS runs on: should a missing scanner instead stop the image with a safe-state code, like the display's 8 and 9?

## Hardware-session checklist: USB AUDIO mode (OS-006d, for `hw-session`)

Image: the default build, with `diag` open. Nothing of this has run on a unit. ADR 0012 "Validation for the hw-session" items 2-8 apply; these are the first steps.

| Id | Steps | Expect | Capture |
|---|---|---|---|
| AUD-1 | `diag usb mode audio`, then reopen the terminal on Linux; `lsusb -v -d 16d0:0e9f`, `arecord -l`, `aplay -l`, `amidi -l`. | the device re-enumerates with bcdDevice 0x0101; a UAC2 card "Track3r Audio" with 8 capture channels named S1..S6, Master L, Master R and 2 playback channels (a boot presents "8 OUT / 2 IN", profile 8; `usb profile 10` gives 10 capture channels S1..S8, Master L, Master R: repeat with it); the MIDI port and the CDC port are still there. | `lsusb -v`, `dmesg` |
| AUD-2 | `usb audio` while a DAW (or `arecord -D hw:... -c 8 -r 44100 -f S16_LE`, `-c 10` after `usb profile 10`; `o3audio.py record` asks the card) records, with the demo pattern playing. | stems streaming 8 ch (10 with profile 10); size44/size45 both non-zero, `other 0`; `underruns 0`, `dropped-blocks 0`, `missed 0`; rate within about 400 ppm of nominal (the engine's 44,117.65 Hz is +400 ppm against 44,100); feedback only when the return runs. | `usb audio` before and after 10 minutes |
| AUD-3 | Record the channels (`usb profile 10` first for the 10 of this row) and look at them: with the pattern playing, S1..S8 carry the tracks (mono, pre-pan), Master L/R the mix. `usb map 1 T1+T2`, `usb map 2 -`. | the map changes at once; an empty slot is digital silence; tracks without a slot stay in the master. | the recording |
| AUD-4 | Play a 44.1 kHz stereo file to the Track3r playback device (no `diag mon` needed since OS-006e: the return has its own route into MAIN, RETURN LEVEL 100 = unity on the Config USB page); `usb audio`. The master stems of the capture (channels 9 and 10 of profile 10) stay silent while it plays: no loopback. | the file is heard on the line out / headphones at the level it was sent, with the monitor at 0; `out ... underruns 0`, `dropped-frames 0`, fill about 200 frames; feedback steady (`trim` near 0); a 48 kHz session works (the host resamples). | `usb audio`, listen |
| AUD-5 | `usb profile 8`, repeat AUD-1 on Windows 10/11 in shared mode, then `usb profile 10`. | the 8-channel profile enumerates and works in shared mode; the 10-channel one is checked against the card's Windows caveats. | notes |
| AUD-6 | `usb mode standard` and back, five times; unplug and replug in each mode; `fw` and MIDI still work in both. | no stale driver binding; every switch ends enumerated; `diag cpu` shows no underruns across a switch; the `usb` counters show no `stray` tokens and no `resyncs`. | `diag usb`, `diag cpu` |
| AUD-7 | `usb audio`: `isr: worst-cycles` (in `usb`) while `usb profile 10 duplex` (10 channels and the return stream, the worst case), and the I2S jitter in `diag cpu`. | worst USB interrupt well under 7.5 us x 180 (about 1,400 cycles for the interleave) plus the existing 1,850; no new underruns; MIDI timing unchanged. | `usb`, `cpu` |
| AUD-8 | OS-USBRET, Linux behind the same two hubs / dock that refused profile 10 duplex: `usb profile 10 out` (**"10 OUT"**), reopen the terminal, `lsusb -v -d 16d0:0e9f`, `arecord -D hw:... -c 10 -r 44100 -f S16_LE`; then `usb profile 8 duplex` (**"8 OUT / 2 IN"**) and `aplay` + `arecord` open at the same time; then `usb profile 10 duplex` for the comparison. | "10 OUT": a UAC2 card with 10 capture channels and **no playback device**, 6 interfaces, MIDI still alive (`amidi -l`), `usb audio` shows `return idle`/`out: starts 0`; "8 OUT / 2 IN": both open, `out ... underruns 0`; "10 OUT / 2 IN": records what happens (today: playback refuses to open). | `lsusb -t`, `lsusb -v` interface list, `usb audio`, the hub topology |
| AUD-9 | OS-USBRET: Config > USB (F5 on the Config screen): USB RETURN Off then On while in AUDIO; `usb return`; switch from the console and read the row back. | the unit re-enumerates on each change; the row shows what `usb return` says and the reverse within a pass; macOS and Windows enumerate both layouts (MIDI at its new interface numbers). | notes |
| AUD-10 | OS-006e: with a DAW or `aplay` playing a -6 dBFS 1 kHz tone into the Tracker, the monitor at 0 (`mon off`) and a project loaded (master DRY 86 is the project default): listen on the line out / headphones and read `mix` (the MST meters). Capture the 10 channels while it plays (the DAW monitoring the Tracker's input). Then Config > USB > RETURN LEVEL 50 (-20 dB), 0 (silence), Delete (back to 100); and `mon on` with a -6 dBFS line-in tone. | the DAW is heard by default at -6 dBFS (+-0.5 dB, `mix`: MST L/R peak; MAIN L/R read 0); the capture's master channels and every stem stay silent (no feedback loop); RETURN LEVEL 50 reads -26, 0 is silent, 100 is as sent; `mon on` of the -6 dBFS line-in reads -18 dBFS on MAIN, whatever the Mixer's DRY level; the recorder's USB source with MONITOR on is not doubled (-6, not 0) and switching to it makes no click | `mix`, o3audio |
| AUD-11 | OS-MIDISLV: CLOCK IN = USB (or CLOCK MODE Slave), a project loaded and the Pattern screen shown; the computer sends a MIDI clock at 120 and Start, then Stop, then Continue (`midi_clock_tool.py send ... --start --stop`). Repeat on the Song screen. | on Start (from the beginning) the strip says PLAY, the BPM follows the master (120.0, not the project's), the playhead runs on the pattern in view (on the Song screen: the song from position 0) and it sounds; on Stop the strip says STOP; on Continue it plays on from where it stopped (a Continue before anything has run starts like a Start); the Play key during the run stops it | `state` (`playing=1 seq=1`), `shot` |
| AUD-12 | OS-USBAUDIO: power the unit with the USB cable already in and a card with no `Settings/device.o3st` (or one written before the setting: USB AUDIO reads On); no console command. `lsusb -v -d 16d0:0e9f`, `dmesg`, `arecord -l`, `aplay -l`, then `aplay` a tone and `arecord` 2 s. Then `diag cpu reset`, start `arecord -D <the card> -c 8 -f S16_LE -r 48000 /dev/null` and, with that capture stream open for 10 s, read `diag cpu` (the stage columns, the fault latch); stop it and read `diag cpu` once more with the stream closed. Then `diag log`. | the host sees **one** enumeration, as the audio device (bcdDevice 0x0101, a UAC2 card "Track3r Audio", 8 capture channels and the 2 playback ones), with the MIDI and serial ports; `aplay` and `arecord` work with no `usb mode audio`; the log says `usb: pull-up on, AUDIO` once and no `usb: detached for a mode change`; the boot takes no longer than the card's mount (tens of ms); with the capture stream open the fault latch stays 0 (no underrun, late drop or slip) and the render max stays inside the budget of G-2 (156,672 cycles); the two `diag cpu` readings (stream open, closed) give the cost of the open stream. | `dmesg`, `diag cpu` (both), `diag log` |
| AUD-13 | OS-USBAUDIO: Config > USB (F5): turn USB AUDIO to Off; look at the host for 5 s; press F7 once, then F7 again; then power-cycle. | the encoder turn alone never reconnects (the row shows `Off*` in the accent colour, the info line says "chosen, not applied yet", the host sees nothing); the first F7 says "USB WILL RECONNECT: PRESS AGAIN"; the second reconnects the unit once as STANDARD (no audio card, MIDI and serial present) and the row reads Off; after the power cycle the unit comes up as STANDARD (`diag log`: `usb: pull-up on, STANDARD`) and Config still says Off. Then On + apply: back as the audio device, once. | notes, `diag log` |
| AUD-14 | OS-USBAUDIO: USB AUDIO and USB RETURN both changed on the page and applied by one F7 F7; a turn, then another page (F1) or another screen, then back; the console's `usb mode standard` with USB AUDIO On, then a reboot. | one re-enumeration for both; leaving the page without applying discards the starred values (nothing reconnects); `usb mode standard` does not change the row and a reboot restores the audio device. A project saved with the other mode in its `USBS` does not change the mode after its load (only its profile and map apply). | notes |

## Capture and the FM radio (OS-011c, `src/capture.cpp`, `src/si4703.cpp`, the render side in `audio`)

The sample recorder (docs/design/product-ux.md section 4, the Record screen) records line in, microphone, radio and the USB return, or what the engine plays, while the pattern keeps
sounding. The platform's part is the control side, on the main loop; the audio is the engine's.

**What is where.** `hal/capture.h` is the seam (`CaptureHw`: `select_source`, `set_gain`, the radio's `power / region / tune / seek / service / status`). `src/capture.cpp` implements it:
the codec's input switch and gain as register plans (`sgtl5000.h` "The analogue input": level first, input select last, `CHIP_ANA_CTRL` always composed with the output mutes, and
`audio.cpp` keeps the ADC's select across `unmute()` and `mute()`), the analogue switch PTE26, and the Si4703 (`si4703.h`, a state machine over I2C0). The device's 130 bytes live in the
SDRAM arena's tail (`app_main.cpp`), not SRAM_U. `service()` is called once a pass of the main loop and does at most one I2C transfer.

**The analogue switch (a correction).** PTE26 (`board.h kInputSourceSelect`) is assumed HIGH for the line jack and LOW for the radio (no evidence from this tree's hardware yet: the open item below). This README and `board.h` said the opposite until OS-011c. The boot level is LOW: the radio's position, with
the radio in reset, so nothing is heard until a source is chosen. **Open: is the line jack heard in the Mixer (`diag mon`) before the Record screen was ever opened?** If it is, the position
is the other way round on this board.

**The radio** follows AN230 and the Si4703 data sheet (the sequence, the values, the four regions): reset with SDIO (the bus's SDA, `i2c::sda_hold`) low, TEST1 0x8100, 500 ms for the
oscillator, POWERCFG, 110 ms, region and volume 10, tune and seek as STC handshakes (bounded: 2 s, 12 s). The chip is forced mono. RDS is not driven (the station name is a
follow-up). **The wiring (reset PTE27, SDIO = SDA, address 0x10) is the board's as far as known and has not been seen on this tree's hardware: no answer is "NO RADIO" on the screen and the rest of the
recorder works without it.** `I2cBus::read` is a read with no address phase (the chip has no register address on the wire).

**The ring contract (render to main loop).** There is no second ring: `RecTap` (`audio/taps.h`) writes the take straight into the sample arena, as ADR 0013 D4 says. The state is one word
with one writer at a time (Idle to Armed or Waiting: main loop; Waiting to Armed, Waiting or Armed to Done: render; Done to Idle: main loop); the frame count is published release and read
acquire, so the main loop reads only frames that are in memory; the memory is the arena's free tail, held by `SampleBank::take_begin` so that nothing else takes it, and never memory a voice
reads. The render adds, per block while a take runs, one scalar-or-SIMD mono mix of at most 64 frames and two atomic stores. A take armed for a step waits (Waiting) for the sequencer's Step event,
whose sample offset in the block the engine hands to `RecTap::on_step` before the graph renders that block: the take's first frame is that step's first sample.

### Hardware-session checklist: capture and radio (OS-011c, for `hw-session`)

Image: the default build. Nothing of this has run on a unit.

| Id | Steps | Expect | Capture |
|---|---|---|---|
| REC-1 | Open Record (SAMPLE REC). With a line source at the jack, play a tone at about -12 dBFS, raise GAIN. | the meters follow the gain (the ADC's 1.5 dB steps), no click when the screen opens (the input is held silent for 200 ms), the line is the mean of L and R. | the meters, a note of the steps |
| REC-2 | Ask: is the line jack heard in the Mixer (`diag mon on`) before Record was opened? Then open Record and look again. | if heard before: PTE26's polarity is the opposite of the assumption (see above). | yes or no |
| REC-3 | MIC LG and MIC HG with a microphone: raise GAIN from 0. | a signal, left channel only, in both ranges; the 200 ms settle after the source change hides the switch click. | the meters |
| REC-4 | RADIO page: FREQUENCY, REGION, F5 / F6 seek. | `RADIO LISTENING`, a station on seek, the signal figure moves, the audio is heard with MONITOR on; "NO RADIO" means the chip did not answer (then the wiring or the reset line is not as assumed: probe SDA during the reset). Try JAPAN with a 76 to 90 MHz station. | the status line, the stations found |
| REC-5 | USB: `diag usb mode audio`, stream a file from the host, source USB. | the take is the stream (mixed to mono), the codec's input is not in it; with the source LINE and the host streaming, the stream is NOT in the take. | the take, by ear |
| REC-6 | RESAMPLE: MASTER, TRACK n, REC BUS while the demo pattern plays. | MASTER is what is heard, before OUT MODE; TRACK n is that track alone (post-fader, mono mean of its pan); REC BUS follows the Routing page. | the takes, by ear |
| REC-7 | Play the demo pattern, START ON BAR, press F1 on the downbeat or earlier. | the take waits (ARMED), starts at the next bar of the pattern; save it and play it next to the pattern: it lines up to a few samples (an external source lags by the codec's round trip of about three blocks, which is not compensated). | the offset against the click, in samples |
| REC-8 | Record with the pattern playing, MONITOR on, then off. | the pattern never stops or glitches (`diag cpu`: no underruns); the monitor is heard only when ON; the take has the input either way. | `diag cpu` before and after |
| REC-9 | SAVE after a trim, stop the pattern, play the new instrument from a pad. | it sounds when the playback is stopped and the pads are let go (the sample bank's handover); the old instrument's sound is not disturbed by the recording. | by ear |
