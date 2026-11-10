# SPDX-License-Identifier: GPL-3.0-or-later
# platform/mk66 (OS-007b): the MK66FX1M0 platform layer and the skeleton image. Implements the HAL contracts; nothing below
# `app` includes it. See README.md.
#
# Two parts:
#   * the pure parts (memory map, crash record, memory tests, clock and SDRAM register values, blink codes, interrupt
#     map) as the static library `platform_mk66_pure`, built on every platform so that the host tests and the QEMU suite
#     (tests/*_test.cpp here, registered in tests/CMakeLists.txt) run them;
#   * with -DO3OS_PLATFORM=mk66, the image: `o3os.elf` linked with the generated mk66.ld, `o3os.hex` / `o3os.bin`, the
#     static checks of the HEX (a failing image is deleted) and their ctests.

set(_mk66_tools "${CMAKE_CURRENT_SOURCE_DIR}/tools")
set(_mk66_header "${CMAKE_CURRENT_SOURCE_DIR}/include/o3os/platform/mk66/mem_map.h")
set(_mk66_template "${CMAKE_CURRENT_SOURCE_DIR}/mk66.ld.in")

o3os_library(platform_mk66_pure SOURCES src/crash_record.cpp src/si4703.cpp DEPENDS hal audio midi)  # midi: midi_config.h (OS-005c2) names the DIN timing constants

# OS-PERF3: the SDRAM's second address at 0x0800_0000 that the LMEM code cache can serve (mem_map.h kSdramAliasBase). THE ONE PLACE OF ITS DEFAULT: the option, handed to every target that
# links platform_mk66_pure (the image and the host and QEMU tests) as the macro of the same name; boot.cpp and sdram_view.h #error when it is not defined. Declared before the
# non-image `return()` below, so that the tests see the same value as the image. Undo for an A/B on the unit: -DO3OS_SDRAM_ICODE_ALIAS=OFF.
option(O3OS_SDRAM_ICODE_ALIAS "Image: after the SDRAM tests, make the controller answer at the ICode alias 0x0800_0000 as well as at 0x7000_0000 (sdramc_config.h kCm0WithAlias), and let the core work in the SDRAM through it (sdram_view.h)" ON)
target_compile_definitions(platform_mk66_pure PUBLIC O3OS_SDRAM_ICODE_ALIAS=$<BOOL:${O3OS_SDRAM_ICODE_ALIAS}>)

# OS-007c: the FT812 display driver's pure parts join the same library (see display/CMakeLists.txt).
add_subdirectory(display)

# OS-007d: the input drivers' pure parts (the key scanners, the debounce, the encoder, the scan, the pad LED driver) join the same library
# (see input/CMakeLists.txt).
add_subdirectory(input)

# OS-006b: the USB0 driver's pure parts (register facts, clock arithmetic, the unique-ID serial, the platform's diag command texts) join the
# same library (see usb/CMakeLists.txt).
add_subdirectory(usb)

# OS-007e: the read-only SD card's pure parts (the SD protocol and card logic, the SDHC facts, the FatFs-backed storage, `diag sd` and `ls`) join the same
# library (see storage/CMakeLists.txt).
add_subdirectory(storage)

# OS-006c: the firmware update path (path guard, streaming HEX validator, transfer, `fw` command, the FatFs-backed FirmwareStore) joins the same library
# (see update/CMakeLists.txt).
add_subdirectory(update)

# OS-007n: the card's write side for saves (the FatFs-backed hal::storage::StorageWriter) joins the same library (see save/CMakeLists.txt).
add_subdirectory(save)

# OS-POWER P1: the shutdown a hold of the Power switch starts (the PowerService state machine over a PowerHost) joins the same library (see power/CMakeLists.txt).
add_subdirectory(power)

# OS-FLASHDIET P5/P8: the long console and bench texts as deflate blobs plus their ids (see text/CMakeLists.txt); the pools and text() are image only (below).
add_subdirectory(text)

# The python tools are stdlib only and need no target: their own self-tests, and the generated linker script against the
# header it came from (the MK66 build below checks the real generated file as well).
if(O3OS_PLATFORM STREQUAL "host" OR O3OS_PLATFORM STREQUAL "mk66")
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME mk66_gen_ld_selftest COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/gen_ld.py --self-test)
  add_test(NAME mk66_image_selftest COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/check_image.py --self-test)
  add_test(NAME mk66_ramfunc_selftest COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/check_ramfunc.py --self-test)
  # OS-006b: the host tool for the `diag` console, against a pseudo-terminal that plays the unit (POSIX hosts; no hardware).
  add_test(NAME mk66_o3console_selftest COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/o3console.py --self-test)
  # OS-HIL: the screen decoder and compare (o3shot.py), the PC-side audio check (o3audio.py: arecord stand-in on a temporary path) and the scenario runner (o3hil.py: a pseudo-terminal that plays
  # the unit), all stdlib only and without hardware. o3console.py's own self-test covers `--shot` through its fake unit.
  add_test(NAME mk66_o3shot_selftest COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/o3shot.py --self-test)
  add_test(NAME mk66_o3audio_selftest COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/o3audio.py --self-test)
  # OS-USBRECQA: the PC-side tone for the USB record scenario (o3tone.py: stand-ins for aplay, pw-play and pw-dump on a temporary path; nothing is played).
  add_test(NAME mk66_o3tone_selftest COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/o3tone.py --self-test)
  add_test(NAME mk66_o3hil_selftest COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/o3hil.py --self-test)
  # OS-USBDISK U1d: the PC side of the USB disk scenarios (o3disk.py: the unit's disk found in a fake /sys, the marked-card proof, the write guard), without hardware.
  add_test(NAME mk66_o3disk_selftest COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/o3disk.py --self-test)
  # Only boot::dma_init() writes DMA_CR and the DCHPRIs (ADR 0013 D1): a source check over this tree, so the host build runs it too.
  add_test(NAME mk66_dma_owner_selftest COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/check_dma_owner.py --self-test)
  add_test(NAME mk66_dma_owner COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/check_dma_owner.py check --root ${CMAKE_CURRENT_SOURCE_DIR})
  # OS-FLASHDIET I3: the importers leave the image: the map must not name formats_tracker, and the only formats::mod / formats::med
  # symbols in it are the probe side that stays in formats (sniff_kind, its two helpers, the two drop_group_name).
  add_test(NAME mk66_map_importers_selftest COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/check_map_importers.py --self-test)
  if(NOT O3OS_PLATFORM STREQUAL "mk66")
    add_test(NAME mk66_ld COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/gen_ld.py check
             --header ${_mk66_header} --template ${_mk66_template})
  endif()
endif()

if(NOT O3OS_PLATFORM STREQUAL "mk66")
  o3os_label_tests()
  return()
endif()

# ---- The image ------------------------------------------------------------------------------------------------------
option(O3OS_MK66_FAULT_DRILL "Skeleton image: raise one deliberate UsageFault 10 s after power-up, to prove the crash record round trip on hardware (a separate image for the first hardware session)" OFF)
option(O3OS_AUDIO_TEST "Skeleton image: the HW-A variant of OS-005a. After the audio path has run cleanly for about 0.6 s the outputs are unmuted and a 1 kHz tone at -20 dBFS fades in; the app has no sound of its own. OFF (the default image): the outputs are muted at reset and open once at boot, after the SD mount, the project load and 0.6 s of clean audio (OS-OUTON), so Play starts the demo pattern at once" OFF)
option(O3OS_AUDIO_TEST_STALL "With O3OS_AUDIO_TEST: 20 s after the tone starts, one render overruns on purpose (3.5 blocks): three underruns, the mute, the recovery, for the HW-A check that left and right survive an underrun. The LED then shows the sticky fault code" OFF)
if(O3OS_AUDIO_TEST_STALL AND NOT O3OS_AUDIO_TEST)
  message(FATAL_ERROR "O3OS_AUDIO_TEST_STALL needs O3OS_AUDIO_TEST=ON")
endif()
option(O3OS_MK66_SDRAM_FULL_TEST "Skeleton image: test every SDRAM cell at boot (a few seconds by estimate); the wiring tests always run" ON)
# OS-PERF3. The LMEM code cache (lmem.h) at boot, and the SDRAM's second address at 0x0800_0000 that the cache can serve (mem_map.h kSdramAliasBase). Both can be undone at run time or
# at build time for an A/B on the unit: `diag cache off`; -DO3OS_CODE_CACHE=OFF / -DO3OS_SDRAM_ICODE_ALIAS=OFF (the alias option is declared at the top of this file).
option(O3OS_CODE_CACHE "Image: turn the K66's 8 KiB code cache on in the reset sequence so that flash code and constants run at cache speed. OFF: left off (the 0.4.6 behaviour); `diag cache off` switches it at run time" ON)

# SGTL5000 CHIP_I2S_CTRL (sgtl5000.h): 0x0130, the value proven on this board (SCLK 32 Fs in the register, 64 Fs on the wire), or 0x0030, the data
# sheet's letter for a 64 Fs slave: the A/B item E-1 of the HW-A checklist. Applied to every compile that includes the header.
set(O3OS_SGTL5000_I2S_CTRL "0x0130" CACHE STRING "SGTL5000 CHIP_I2S_CTRL value: 0x0130 (default, the value proven on this board) or 0x0030 (SCLKFREQ 64 Fs, HW-A item E-1)")
set_property(CACHE O3OS_SGTL5000_I2S_CTRL PROPERTY STRINGS 0x0130 0x0030)
if(NOT O3OS_SGTL5000_I2S_CTRL MATCHES "^0x0[01]30$")
  message(FATAL_ERROR "O3OS_SGTL5000_I2S_CTRL must be 0x0130 or 0x0030 (got '${O3OS_SGTL5000_I2S_CTRL}')")
endif()
target_compile_definitions(platform_mk66_pure PUBLIC O3OS_SGTL5000_I2S_CTRL=${O3OS_SGTL5000_I2S_CTRL})

# The linker script, generated from mem_map.h at build time.
set(_mk66_ld "${CMAKE_CURRENT_BINARY_DIR}/mk66.ld")
add_custom_command(OUTPUT ${_mk66_ld}
  COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/gen_ld.py generate --header ${_mk66_header} --template ${_mk66_template} --out ${_mk66_ld}
  DEPENDS ${_mk66_tools}/gen_ld.py ${_mk66_template} ${_mk66_header}
  COMMENT "Generating mk66.ld from mem_map.h"
  VERBATIM)
add_custom_target(mk66_ld_script DEPENDS ${_mk66_ld})

# The image: startup, boot steps, crash store, clock and main on top of the pure library and the HAL contracts. One
# executable of first-party objects, no archive in between: nothing the vector table names can be dropped.
add_executable(o3os src/startup.cpp src/boot.cpp src/crash_store.cpp src/clock.cpp src/i2c.cpp src/audio.cpp src/capture.cpp src/midi_din.cpp src/cache.cpp src/main.cpp)
# OS-BENCHOPT: the CPU bench (`diag bench ...`, `diag cache check`; src/bench.cpp, about 17.8 KB of flash, no SRAM_L since OS-CEWTENV W3a: its load kernel runs from flash) is a measurement tool for the tester's bench images, not part of a
# release image: OFF by default. ON (-DO3OS_BENCH=ON) is how a bench image is built (tools/ci.sh builds one, `mk66-bench`, so that it does not rot). With it OFF the console still lists
# `bench`, and says so when asked. The host tests of the bench's pure half (bench_report.h, cache_check.h) do not depend on the option.
option(O3OS_BENCH "Image: include the CPU bench (diag bench <kernel|all>, diag cache check; src/bench.cpp). OFF (default): a release image carries none of it" OFF)
if(O3OS_BENCH)
  target_sources(o3os PRIVATE src/bench.cpp)
  # OS-CPUOPT1: the bench's reference copies of the SVF loop (`diag bench cpuopt`) must be compiled as the audio library is, -ffp-contract=off (audio/CMakeLists.txt): without it GCC fuses their plain products into VFMA, which rounds once, and `cpuopt_eq_svf` compares the new loop with a filter the voices never ran (the first unit run said FAIL for exactly this).
  set_source_files_properties(src/bench.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()
# OS-FLASHDIET B1: the retired one-off A/B benches (`bench cpuopt`, `looper`, `voice_st`, `disk` and `bench disk reset|clean`; the four groups of src/bench.cpp, about 11-13 KB of flash) are kept
# for the image that needs one: ON (-DO3OS_BENCH_ARCHIVE=ON, with O3OS_BENCH=ON) compiles them into a bench image (tools/ci.sh builds one, `mk66-bench-archive`, so that they do not rot). OFF
# (default): a bench image carries none of them; the console still lists the four kernels and answers `not in this image (O3OS_BENCH_ARCHIVE)`. A release image (O3OS_BENCH=OFF) carries no bench at all.
option(O3OS_BENCH_ARCHIVE "Bench image: keep the retired one-off A/B benches (bench cpuopt|looper|voice_st|disk) in the image. OFF (default): they are compiled out and the console answers not in this image (O3OS_BENCH_ARCHIVE)" OFF)
# OS-006b: the USB0 device driver and the `diag` console on the CDC port (usb/src), with the build identity `diag version` prints. The
# identity header is written at build time (tools/gen_build_info.cmake: git hash, "-dirty" if the tree differs), only when it changed.
set(_mk66_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/gen")
add_custom_target(o3os_build_info
  COMMAND ${CMAKE_COMMAND} -DSRC=${PROJECT_SOURCE_DIR} -DOUT=${_mk66_gen_dir}/o3os_build_info.h -P ${_mk66_tools}/gen_build_info.cmake
  BYPRODUCTS ${_mk66_gen_dir}/o3os_build_info.h
  COMMENT "o3os_build_info.h"
  VERBATIM)
add_dependencies(o3os o3os_build_info)
target_include_directories(o3os PRIVATE ${_mk66_gen_dir})
target_sources(o3os PRIVATE usb/src/usb0.cpp usb/src/console.cpp text/src/text.cpp)
# OS-007e: the read-only SD card. The SDHC register policy (storage/src/sdhc_bus.cpp) and the bus, card and volume objects (storage/src/device.cpp; the project loader is a static of src/app_main.cpp)
# join the image; the SD protocol, the FatFs-backed storage and the `diag sd` / `ls` text are in platform_mk66_pure, FatFs in third_party/fatfs.
option(O3OS_SD_HIGH_SPEED "Image: switch the SD card to high speed (CMD6, 50 MHz asked, 45 MHz made) after the 25 MHz identification. OFF: stay at 22.5 MHz, for a hardware session that sees CRC errors at the higher clock" ON)
target_sources(o3os PRIVATE storage/src/sdhc_bus.cpp storage/src/device.cpp)
# OS-006c: the firmware update path's image half (update/src/fw_host_mk66.cpp: the FwHost with the transport stop and the mute, the PTA1 hand-over pin and the
# entropy of the token; the FatFs-backed FirmwareStore, the `fw` service and the HEX validator are in platform_mk66_pure). OS-ANYCARD (maintainer decision 2026-10-17): neither
# the firmware writer (`fw put` / `rm` / `arm` / `update`) nor the saves look for a marker file: any writable FAT12/16/32 card is written (the firmware writer also needs a Firmware/
# folder). Every other safety stays: path guard, name rules, read-back, HEX validation, CRC, token, hand-over. The options O3OS_FW_REQUIRE_TEST_CARD and O3OS_SAVE_REQUIRE_TEST_CARD are gone.
target_sources(o3os PRIVATE update/src/fw_host_mk66.cpp)
# OS-POWER P1: the image's PowerHost (power/src/power_host_mk66.cpp): the engine and audio stop, the card, USB, the lights and the restart. THE ONLY SOURCE THAT NAMES board::kPowerSwitch's pull and reads PTC3.
target_sources(o3os PRIVATE power/src/power_host_mk66.cpp)
# OS-POWER P2: the end of a real off (a hold of Power, `diag power off`) is the VLPR off: PTC3 polled in very-low-power RUN, a press is a software reset. OFF: the end is P1's restart after the release (the
# fallback of the decision "if the wake fails on the unit the alpha ships P1's restart-off only", spec section 1). `diag power off dry` is the restart in both. One place of the default.
option(O3OS_POWER_VLPR_OFF "Image: the end of a real power-off is the VLPR off with a polled wake (OS-POWER P2). OFF: a restart after the release of Power (P1)" ON)
target_compile_definitions(o3os PRIVATE O3OS_POWER_VLPR_OFF=$<BOOL:${O3OS_POWER_VLPR_OFF}>)
target_compile_definitions(platform_mk66_pure PRIVATE O3OS_SD_HIGH_SPEED=$<BOOL:${O3OS_SD_HIGH_SPEED}>)
target_link_libraries(o3os PRIVATE usb)
target_link_libraries(o3os PRIVATE platform_mk66_pure audio midi hal core)
# OS-FLASHDIET-ROOM R3: the TB-303, chip, FM and SID modules are no longer in the image (they were o3m_builtin, OS-012d); the SD content zip carries them (tools/release/make_starter.py).
o3os_strict_warnings(o3os)
target_compile_definitions(o3os PRIVATE
  O3OS_FAULT_DRILL=$<BOOL:${O3OS_MK66_FAULT_DRILL}>
  O3OS_SDRAM_FULL_TEST=$<BOOL:${O3OS_MK66_SDRAM_FULL_TEST}>
  O3OS_CODE_CACHE=$<BOOL:${O3OS_CODE_CACHE}>
  O3OS_BENCH=$<BOOL:${O3OS_BENCH}>
  O3OS_BENCH_ARCHIVE=$<BOOL:${O3OS_BENCH_ARCHIVE}>
  O3OS_AUDIO_TEST=$<BOOL:${O3OS_AUDIO_TEST}>
  O3OS_AUDIO_TEST_STALL=$<BOOL:${O3OS_AUDIO_TEST_STALL}>)
# OS-007c: the display. The FT812 driver's hardware half (DSPI2 as the link, the eDMA engine, the bring-up, display/src) and the
# image's composition root (src/app_main.cpp: the OS app over the real display, the one platform file allowed to include the
# app and ui layers) join the image; the app and its libraries link in.
option(O3OS_DEMO_CYCLE "Image: walk the pattern cursor down and cycle the three themes every few seconds, for the hardware session (no input exists yet)" OFF)
set(O3OS_DISPLAY_ROTATE "1" CACHE STRING "Image: panel orientation, REG_ROTATE 0..7 (0 landscape, 1 inverted). 1 is right on the maintainer's unit (OS-HW1, `diag rot`); 0 shows it upside down")
if(NOT O3OS_DISPLAY_ROTATE MATCHES "^[0-7]$")
  message(FATAL_ERROR "O3OS_DISPLAY_ROTATE must be 0..7 (REG_ROTATE), got '${O3OS_DISPLAY_ROTATE}'")
endif()
target_sources(o3os PRIVATE src/app_main.cpp display/src/device.cpp)
target_link_libraries(o3os PRIVATE app ui eve seq model)  # seq and model: the sequencer and the demo content of the sound (OS-005d), built in the composition root
# OS-007d: the input drivers' hardware half (pins, the FTM2 quadrature decoder, the scan's board seam; the I2C master it uses is
# src/i2c.cpp, shared with the codec). INPUT_TEST: an image that starts on the INPUT TEST screen with no boot chord.
option(O3OS_INPUT_TEST "Image: start on the hidden INPUT TEST screen without the boot chord (Shift + F1 + F8 at power-up), for the hardware session" OFF)
target_sources(o3os PRIVATE input/src/device.cpp)
target_compile_definitions(o3os PRIVATE O3OS_INPUT_TEST=$<BOOL:${O3OS_INPUT_TEST}>)
target_compile_definitions(o3os PRIVATE
  O3OS_DEMO_CYCLE=$<BOOL:${O3OS_DEMO_CYCLE}>
  O3OS_DISPLAY_ROTATE=${O3OS_DISPLAY_ROTATE})

# OS-FLASHDIET B1: the archive bench image no longer fits the 1 MiB flash with the OS around it (about +11-13 KB over the plain bench image, which was 1012 KB), so `tools/ci.sh mk66` does not link it:
# it builds this target, the two translation units that hold every O3OS_BENCH_ARCHIVE line (src/bench.cpp, usb/src/console.cpp) compiled with the image's own definitions, include paths, options
# and libraries (generator expressions on `o3os`), no link. Not part of ALL, no output. `o3os_image` of an archive configure still links; it fits only on a tree small enough for it.
if(O3OS_BENCH AND O3OS_BENCH_ARCHIVE)
  add_library(o3os_bench_archive_objs OBJECT EXCLUDE_FROM_ALL src/bench.cpp usb/src/console.cpp)
  target_include_directories(o3os_bench_archive_objs PRIVATE $<TARGET_PROPERTY:o3os,INCLUDE_DIRECTORIES>)
  target_compile_definitions(o3os_bench_archive_objs PRIVATE $<TARGET_PROPERTY:o3os,COMPILE_DEFINITIONS>)
  target_compile_options(o3os_bench_archive_objs PRIVATE $<TARGET_PROPERTY:o3os,COMPILE_OPTIONS>)
  target_link_libraries(o3os_bench_archive_objs PRIVATE usb platform_mk66_pure audio midi hal core app ui eve seq model)
  add_dependencies(o3os_bench_archive_objs o3os_build_info)
endif()

# The reset sequence is the C runtime: its copy and zero loops must stay loops, not become memcpy / memset calls.
set_source_files_properties(src/startup.cpp PROPERTIES COMPILE_OPTIONS -fno-tree-loop-distribute-patterns)
set_target_properties(o3os PROPERTIES OUTPUT_NAME o3os SUFFIX .elf)
add_dependencies(o3os mk66_ld_script)
set_property(TARGET o3os APPEND PROPERTY LINK_DEPENDS ${_mk66_ld})
# Our linker script and startup instead of newlib's crt0; newlib-nano and libstdc++ for the few library calls the image
# makes (no libnosys: there is no heap, no stdio and no exit, and a library function that wants one is a link error, which is
# how "no heap after boot" is enforced). Unused sections are dropped; a section the script does not place is an error.
target_link_options(o3os PRIVATE
  -nostartfiles -T${_mk66_ld} --specs=nano.specs
  -Wl,--gc-sections -Wl,--orphan-handling=error -Wl,--print-memory-usage -Wl,-Map=$<TARGET_FILE:o3os>.map)

# HEX and BIN, behind the static checks. The HEX is an OUTPUT of a custom command, so a failed check (which deletes the
# HEX) is repeated by the next build instead of being skipped as "up to date". --set-start=0: no start-address record in the
# HEX (objcopy would write one for the ELF entry point); the check refuses one. --gap-fill 0xFF:
# the linker script asserts there is no hole; this fills one if it ever appeared, and the
# check (`contiguous`) refuses a hole either way.
# OS-005a: the audio ISRs run from SRAM_L, so vector table entries point there (the reset sequence copies `.ramfunc_l` before any
# interrupt is enabled): the image check is told the SRAM_L code region, mem_map.h kSramLBase and kRamfuncLSize (pinned by
# mk66_mem_map_test.cpp), and still refuses every other address outside flash.
set(_mk66_ramfunc_region "0x1FFF0000:0xC000")
# OS-GATE-BOOTCRASH: the pinned layout of `.noinit` (what survives a reset): an object that moves, grows or appears there fails the build until the file is changed on purpose.
set(_mk66_noinit_layout "${_mk66_tools}/noinit_layout.txt")
# OS-BUG-LASTSECTOR: an image must end at or below 0xFF000 (check `last_sector_free`; the unit hung after an update with an image that filled the last
# flash sector). The CI build of a bench image (O3OS_BENCH=ON) is over that bound until the bench gets a flash diet and is not meant for a unit:
# only it waives the check, here and in `mk66_image`; the gate (`check_image.py check` without the flag, skill fw-gate-check) refuses such an image.
set(_mk66_image_flags "")
if(O3OS_BENCH)
  set(_mk66_image_flags --allow-last-sector)
endif()
set(_mk66_hex "${CMAKE_CURRENT_BINARY_DIR}/o3os.hex")
set(_mk66_bin "${CMAKE_CURRENT_BINARY_DIR}/o3os.bin")
add_custom_command(OUTPUT ${_mk66_hex}
  COMMAND ${CMAKE_SIZE} -A -d $<TARGET_FILE:o3os>
  COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/check_ramfunc.py check $<TARGET_FILE:o3os> --objdump ${CMAKE_OBJDUMP}
  COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/check_image.py noinit $<TARGET_FILE:o3os> --nm ${CMAKE_NM} --expect ${_mk66_noinit_layout}
  COMMAND ${CMAKE_OBJCOPY} -O ihex --set-start=0 --gap-fill 0xFF $<TARGET_FILE:o3os> ${_mk66_hex}
  COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/check_image.py check ${_mk66_hex} --expect-sp 0x20030000 --ramfunc-l ${_mk66_ramfunc_region} ${_mk66_image_flags} --delete-on-fail
          --json ${CMAKE_CURRENT_BINARY_DIR}/o3os.image-check.json
  DEPENDS o3os ${_mk66_tools}/check_image.py ${_mk66_tools}/check_ramfunc.py ${_mk66_noinit_layout}
  COMMENT "o3os.hex and its static image checks"
  VERBATIM)
add_custom_command(OUTPUT ${_mk66_bin}
  COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:o3os> ${_mk66_bin}
  DEPENDS ${_mk66_hex}
  COMMENT "o3os.bin"
  VERBATIM)
add_custom_target(o3os_image ALL DEPENDS ${_mk66_hex} ${_mk66_bin})

# ---- ctest: the static checks of the built image --------------------------------------------------------------------------
# `mk66_flashconfig` carries its own copy of the required 16 bytes (the gate's value, the flashing-safety rules): a wrong
# flash configuration field can permanently lock the MCU, so the post-build check and this test must not share a constant.
add_test(NAME mk66_ld COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/gen_ld.py check
         --header ${_mk66_header} --template ${_mk66_template} --ld ${_mk66_ld})
add_test(NAME mk66_image COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/check_image.py check ${_mk66_hex} --expect-sp 0x20030000
         --ramfunc-l ${_mk66_ramfunc_region} ${_mk66_image_flags})
# OS-005a: the audio hot path (SRAM_L) calls nothing in flash, no veneer in the section.
add_test(NAME mk66_ramfunc COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/check_ramfunc.py check $<TARGET_FILE:o3os> --objdump ${CMAKE_OBJDUMP})
# OS-FLASHDIET I3: over the link map: the image does not link the MOD / S3M / MED importers (formats_tracker), only the probe side
# of them that stays in formats (mod::sniff_kind with tagged_mod_min_bytes and is_soundtracker15, the two drop_group_name).
add_test(NAME mk66_map_importers COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/check_map_importers.py check $<TARGET_FILE:o3os>.map)
add_test(NAME mk66_flashconfig COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/check_image.py flashconfig ${_mk66_hex}
         --expect ffffffffffffffffffffffffdef9ffff)
# OS-GATE-BOOTCRASH: the symbols of `.noinit` in the linked ELF are the pinned layout (the post-build step above runs the same check).
add_test(NAME mk66_noinit_layout COMMAND ${Python3_EXECUTABLE} -I ${_mk66_tools}/check_image.py noinit $<TARGET_FILE:o3os> --nm ${CMAKE_NM}
         --expect ${_mk66_noinit_layout})
# The noinit macros (sections.h) accept what they should and refuse an initialised object at compile time.
add_test(NAME mk66_noinit_guard
  COMMAND ${CMAKE_COMMAND} -DCXX=${CMAKE_CXX_COMPILER}
          "-DFLAGS=-mthumb|-mcpu=cortex-m4|-mfpu=fpv4-sp-d16|-mfloat-abi=hard|-std=gnu++17|-fno-rtti|-fno-exceptions"
          -DINCLUDE=${CMAKE_CURRENT_SOURCE_DIR}/include -DDIR=${CMAKE_CURRENT_SOURCE_DIR}/tests -DWORK=${CMAKE_CURRENT_BINARY_DIR}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/noinit_guard.cmake)
set_tests_properties(mk66_ld mk66_image mk66_ramfunc mk66_flashconfig mk66_noinit_guard mk66_noinit_layout mk66_map_importers PROPERTIES TIMEOUT 60 LABELS mk66)

o3os_label_tests()
