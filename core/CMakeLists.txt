# SPDX-License-Identifier: GPL-3.0-or-later
# core: ring, fixed containers, input event, tick helpers, and the input pipeline's pure parts (gesture
# recogniser, binding resolver, keymap parser), the notification queue and the cooperative job runner, (OS-002d) the parameter
# descriptors with their maths and the MIDI Learn rules, and (OS-FLASHDIET A1) the raw-deflate stream decoder. No heap, depends on nothing.
o3os_library(core SOURCES src/fixed_string.cpp src/instances.cpp src/gesture.cpp src/binding.cpp src/keymap.cpp
          src/notify.cpp src/job.cpp src/fast_math.cpp src/fast_sincos.cpp src/param.cpp src/learn.cpp src/inflate.cpp src/ptfx_math.cpp)
# OS-CEPTFX S2: sin_fast/cos_fast came from audio/src/module_voices.cpp, which the cross builds compile -ffp-contract=off
# (audio/CMakeLists.txt). Keep that unfused arithmetic here too, so a module's render stays bit for bit what it was
# (the goldens of tests/module_builtin_run_test.cpp); exp2_fast/log2_fast in fast_math.cpp stay as core always built them.
if(O3OS_CROSS)
  set_source_files_properties(src/fast_sincos.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()
