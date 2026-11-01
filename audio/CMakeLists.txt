# SPDX-License-Identifier: GPL-3.0-or-later
# audio: the pure audio engine (ADR 0013, OS-005a/b): the slot protocol between the DMA ISR and the render, the render loop with its stage
# stamps, the fixed graph (tracks, buses, routing, master chain, meters, taps; OS-005b) over the sample kernels, the test tone, AudioStatus
# and the `diag cpu` / `diag mem` / `diag mix` formatters. No hardware access (the platforms implement hal::audio::Device); depends on core
# (the rate constant, ParamChange), hal (the contract) and usb (the OS-006a stem ring and interleavers); it does NOT depend on model: the
# graph's track count and parameter ids are its own (graph_params.h), and tests/audio_model_ids_test.cpp holds them equal to the model's.
# No heap, no double, no printf. It sits beside the ranked chain like model and formats: app, platform, sim and tests may
# use it.
o3os_library(audio
  SOURCES
    src/status.cpp
    src/test_signal.cpp
    src/engine.cpp
    src/pipeline.cpp
    src/diag.cpp
    src/kernels.cpp
    src/meters.cpp
    src/loop_player.cpp
    src/loop_session.cpp
    src/metro.cpp
    src/taps.cpp
    src/app_tap.cpp
    src/stem_map.cpp
    src/graph.cpp
    src/voices.cpp
    src/module_voices.cpp
    src/module_governor.cpp
    src/insert_host.cpp
    src/paula.cpp
    src/amiga_voices.cpp
    src/med_tick.cpp
    src/tracker_fx.cpp
    src/paula_tables.cpp
    src/demo_kit.cpp
    src/send_delay.cpp
    src/send_reverb.cpp
    src/master_limiter.cpp
    src/master_dj_filter.cpp
    src/master_scratch.cpp
    src/master_kill_eq.cpp
  DEPENDS core hal usb module)
# meter_level() takes a square root (main loop only): without errno handling it is one VSQRT, and the image does not pull newlib's errno
# machinery (the 80 bytes of `_impure_data` in .data) and a sqrtf for it.
target_compile_options(audio PRIVATE -fno-math-errno)
# OS-SENDFX: the send effects (SND A delay, SND B reverb) run from flash, called through the graph's SendFxFn pointer: the SRAM_L hot budget has no room for them (mem_map.h
# kSramLHotBudget). The target build is -Os everywhere, so their inner loops are asked for -O3 on their own (flash is not the scarce resource, the render's block is).
# OS-CPUOPT1: the loop player (flash, once a block) too: its register loop is -O3 code; at -Os GCC left the inner loop in an out-of-line lambda that went through the stack every frame.
# OS-LIMIT: the master limiter (the chain's limiter stage) is called through the graph's MasterFn pointer from flash the same way, and gets the same. OS-DJ A2: so does the DJ filter (the bass stage); A3: and the kill EQ (the eq stage).
if(O3OS_CROSS)
  set_source_files_properties(src/send_delay.cpp src/send_reverb.cpp src/master_limiter.cpp src/master_dj_filter.cpp src/master_kill_eq.cpp src/master_scratch.cpp src/loop_player.cpp PROPERTIES COMPILE_OPTIONS "-O3")
endif()
# On the MK66 the real-time functions (O3OS_AUDIO_HOT) run from SRAM_L (ADR 0013 D7), where the reset sequence copies them.
if(O3OS_PLATFORM STREQUAL "mk66")
  target_compile_definitions(audio PUBLIC O3OS_AUDIO_RAMFUNC=1)
endif()
# The hot path may not call into flash (tools/check_ramfunc.py): a zeroing or copying loop must stay a loop, not become a memset or
# memcpy call (the same reason platform/mk66's startup.cpp is built with this flag).
if(O3OS_CROSS)
  target_compile_options(audio PRIVATE -fno-tree-loop-distribute-patterns -ffp-contract=off)
endif()
# Host Debug (sanitizer) builds compile the render path with -O2 like the usb library: the property tests push millions of
# blocks through it. Sanitizers stay on.
if(O3OS_PLATFORM STREQUAL "host")
  target_compile_options(audio PRIVATE $<$<CONFIG:Debug>:-O2>)
endif()
