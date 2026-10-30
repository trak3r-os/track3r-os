# SPDX-License-Identifier: GPL-3.0-or-later
# modules: the modules this repository builds (OS-012a, ADR 0002). Each one is a C++ file written against the SDK (module/sdk/o3module.h)
# and is built twice from the same source:
#   - for the host, as a shared object (o3m_<name>_host) the tests load with dlopen and the simulator can load: same ABI, same code;
#   - for the Cortex-M4F, as a .tmod file (o3m_<name>_tmod, part of `all`): tools/o3m_pack.py compiles with the SDK's flags, links with
#     module/sdk/o3module.ld and packs. The file is what goes to /modules on the card; nothing here copies it anywhere.
# The packer needs arm-none-eabi-g++: the cross builds use their own compiler, the host build looks in ARM_GNU_ROOT (CMake variable or environment, see cmake/arm-none-eabi-toolchain.cmake), then on PATH. Without
# it the .tmod targets are not made and the tests that need them say so (O3_CI=1 makes that a failure).
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(O3M_PACK "${PROJECT_SOURCE_DIR}/tools/o3m_pack.py")
set(O3M_SDK_DIR "${PROJECT_SOURCE_DIR}/module/sdk")

if(O3OS_CROSS)
  set(O3M_ARM_CXX "${CMAKE_CXX_COMPILER}")
else()
  set(_o3m_hints)
  foreach(_o3m_root IN ITEMS "${ARM_GNU_ROOT}" "$ENV{ARM_GNU_ROOT}" "$ENV{O3_ARM_TOOLCHAIN}")
    if(_o3m_root)
      list(APPEND _o3m_hints "${_o3m_root}/bin")
    endif()
  endforeach()
  find_program(O3M_ARM_CXX NAMES arm-none-eabi-g++ HINTS ${_o3m_hints})
endif()

# The manifest of a module (module.json, tools/o3m_gen.py): o3m_manifest(<name> <manifest> [GENERATE])
#   - <build>/gen/<name>.expect: what a loader must read back from the built file (tests/module_gen_test.cpp compares it with the real loader's reading);
#   - the ctest o3m_gen_verify_<name>: the built .tmod read with o3m_gen.py's own reader against the manifest (when the cross compiler built it);
#   - GENERATE: <build>/gen/<name>_gen.h, the descriptor header the module's source includes (o3m_add_module(... GEN) puts that directory on its include path);
#     without it the header is the source tree's own (committed: ctest o3m_gen_check_<name> finds it stale).
set(O3M_GEN "${PROJECT_SOURCE_DIR}/tools/o3m_gen.py")
set(O3M_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen")
function(o3m_manifest name manifest)
  cmake_parse_arguments(ARG "GENERATE" "" "" ${ARGN})
  file(MAKE_DIRECTORY "${O3M_GEN_DIR}")
  set(abs_manifest "${CMAKE_CURRENT_SOURCE_DIR}/${manifest}")
  set_property(GLOBAL PROPERTY O3M_MANIFEST_${name} "${abs_manifest}")
  set(outputs "${O3M_GEN_DIR}/${name}.expect")
  add_custom_command(OUTPUT "${O3M_GEN_DIR}/${name}.expect"
    COMMAND ${Python3_EXECUTABLE} -I ${O3M_GEN} expect ${abs_manifest} --out "${O3M_GEN_DIR}/${name}.expect"
    DEPENDS ${abs_manifest} ${O3M_GEN} COMMENT "o3m_gen ${name}.expect" VERBATIM)
  if(ARG_GENERATE)
    list(APPEND outputs "${O3M_GEN_DIR}/${name}_gen.h")
    add_custom_command(OUTPUT "${O3M_GEN_DIR}/${name}_gen.h"
      COMMAND ${Python3_EXECUTABLE} -I ${O3M_GEN} gen ${abs_manifest} --out "${O3M_GEN_DIR}/${name}_gen.h"
      DEPENDS ${abs_manifest} ${O3M_GEN} COMMENT "o3m_gen ${name}_gen.h" VERBATIM)
  else()
    get_filename_component(dir ${abs_manifest} DIRECTORY)
    add_test(NAME o3m_gen_check_${name} COMMAND ${Python3_EXECUTABLE} -I ${O3M_GEN} check ${abs_manifest} "${dir}/${name}_gen.h")
  endif()
  add_custom_target(o3m_${name}_gen ALL DEPENDS ${outputs})
  if(O3M_ARM_CXX AND NOT O3OS_CROSS)
    add_test(NAME o3m_gen_verify_${name} COMMAND ${Python3_EXECUTABLE} -I ${O3M_GEN} verify ${abs_manifest} "${CMAKE_CURRENT_BINARY_DIR}/${name}.tmod")
  endif()
endfunction()

# o3m_add_module(<name> SOURCES <file>... [OPT <-O2>] [VERSION <X.Y.Z>] [CAPS <a,b,...>] [INCLUDE <dir>...] [GEN])
# VERSION (OS-MODVER) is the module's own version, written into the .tmod header by the packer (o3m_pack.py --version): the OS prefers a card copy to the built-in module of the same
# name only when the card copy's version is higher. The module must declare ABI 1.2 (module.json "abi": "1.2"); without VERSION the file is unversioned (word 0).
# CAPS (OS-H1, ABI 1.3) is an app's request for host services, comma separated (midi_in, read_project, edit_project; module.json "capabilities" says the same and must match: the verify test
# compares the file with the manifest); the packer writes them to the header (o3m_pack.py --caps).
function(o3m_add_module name)
  cmake_parse_arguments(ARG "GEN" "OPT;VERSION;CAPS" "SOURCES;INCLUDE" ${ARGN})
  if(NOT ARG_OPT)
    set(ARG_OPT -Os)
  endif()
  set(abs_sources "")
  set(src_args "")
  foreach(src IN LISTS ARG_SOURCES)
    list(APPEND abs_sources "${CMAKE_CURRENT_SOURCE_DIR}/${src}")
    list(APPEND src_args --src "${CMAKE_CURRENT_SOURCE_DIR}/${src}")
  endforeach()
  set(inc_args "")
  foreach(dir IN LISTS ARG_INCLUDE)
    list(APPEND inc_args -I "${CMAKE_CURRENT_SOURCE_DIR}/${dir}")
  endforeach()
  set(version_args "")
  if(ARG_VERSION)
    list(APPEND version_args --version ${ARG_VERSION})
  endif()
  if(ARG_CAPS)
    list(APPEND version_args --caps ${ARG_CAPS})
  endif()
  # The packer is a custom command, so CMake does not see the headers the sources include (the DSP header, the generated descriptor): without these, editing chip_dsp.h left a stale chip.tmod
  # in an incremental build. The *.h next to each source and in each INCLUDE directory are dependencies of the packed file (the host shared object tracks its headers by itself).
  set(hdr_deps "")
  foreach(src IN LISTS ARG_SOURCES)
    get_filename_component(src_dir "${CMAKE_CURRENT_SOURCE_DIR}/${src}" DIRECTORY)
    file(GLOB src_headers CONFIGURE_DEPENDS "${src_dir}/*.h")
    list(APPEND hdr_deps ${src_headers})
  endforeach()
  foreach(dir IN LISTS ARG_INCLUDE)
    file(GLOB inc_headers CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/*.h")
    list(APPEND hdr_deps ${inc_headers})
  endforeach()
  set(gen_deps "")
  if(ARG_GEN)  # the manifest's header (o3m_manifest(... GENERATE)) is in the build tree. The builds depend on the manifest (the header is a function of it) and are
    # ordered after the target that writes the header: naming the generated file itself as a dependency would make each target run the generator, concurrently.
    get_property(manifest_path GLOBAL PROPERTY O3M_MANIFEST_${name})
    list(APPEND inc_args -I "${O3M_GEN_DIR}")
    list(APPEND gen_deps "${manifest_path}")
  endif()

  if(NOT O3OS_CROSS)
    add_library(o3m_${name}_host MODULE ${ARG_SOURCES})
    target_include_directories(o3m_${name}_host PRIVATE ${O3M_SDK_DIR})
    if(ARG_GEN)
      target_include_directories(o3m_${name}_host PRIVATE ${O3M_GEN_DIR})
      add_dependencies(o3m_${name}_host o3m_${name}_gen)
    endif()
    foreach(dir IN LISTS ARG_INCLUDE)
      target_include_directories(o3m_${name}_host PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/${dir}")
    endforeach()
    set_target_properties(o3m_${name}_host PROPERTIES PREFIX "" OUTPUT_NAME o3m_${name} CXX_VISIBILITY_PRESET hidden)
    o3os_strict_warnings(o3m_${name}_host)
    target_compile_options(o3m_${name}_host PRIVATE -fsingle-precision-constant -fno-math-errno -ffp-contract=off)  # (the packer builds the .tmod with -ffp-contract=off too: same results on the host and the MK66)
  endif()

  if(O3M_ARM_CXX)
    set(tmod "${CMAKE_CURRENT_BINARY_DIR}/${name}.tmod")
    add_custom_command(OUTPUT "${tmod}"
      COMMAND ${Python3_EXECUTABLE} -I ${O3M_PACK} build --cxx ${O3M_ARM_CXX} --out ${tmod} --elf "${CMAKE_CURRENT_BINARY_DIR}/${name}.elf" --opt=${ARG_OPT} ${inc_args}
              ${version_args} ${src_args}
      DEPENDS ${abs_sources} ${hdr_deps} ${gen_deps} ${O3M_PACK} ${O3M_SDK_DIR}/o3module.h ${O3M_SDK_DIR}/o3module.ld
      COMMENT "o3m_pack ${name}.tmod"
      VERBATIM)
    add_custom_target(o3m_${name}_tmod ALL DEPENDS "${tmod}")
    if(ARG_GEN)
      add_dependencies(o3m_${name}_tmod o3m_${name}_gen)
    endif()
    if(NOT O3OS_CROSS)
      # The relocation oracle: the same module linked at the addresses the loader test relocates to (tests/module_fixture_test.cpp).
      set(raw "${CMAKE_CURRENT_BINARY_DIR}/${name}.raw")
      add_custom_command(OUTPUT "${raw}"
        COMMAND ${Python3_EXECUTABLE} -I ${O3M_PACK} build --cxx ${O3M_ARM_CXX} --out ${raw} ${inc_args} --opt=${ARG_OPT} --hot-at 0x1FFFC000 --cold-at 0x70000000
                ${src_args}
        DEPENDS ${abs_sources} ${hdr_deps} ${gen_deps} ${O3M_PACK} ${O3M_SDK_DIR}/o3module.h ${O3M_SDK_DIR}/o3module.ld
        COMMENT "o3m_pack ${name}.raw (oracle)"
        VERBATIM)
      add_custom_target(o3m_${name}_raw ALL DEPENDS "${raw}")
      if(ARG_GEN)
        add_dependencies(o3m_${name}_raw o3m_${name}_gen)
      endif()
    endif()
  endif()
endfunction()

# o3m_add_test(<name> <source>...): a host test executable of one module, o3m_<name>_test (ctest `o3m_<name>_test`, OS-012c): doctest, the module's host shared object loaded with dlopen
# (O3M_<NAME>_HOST_SO) and driven through the ABI, the views through module::ViewHost (modules/tests/o3m_test_support.h). Host only; the test references are in double precision on purpose.
function(o3m_add_test name)
  if(O3OS_CROSS)
    return()
  endif()
  string(TOUPPER ${name} upper)
  add_executable(o3m_${name}_test ${ARGN})
  target_include_directories(o3m_${name}_test SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party)  # <doctest/doctest.h>
  target_link_libraries(o3m_${name}_test PRIVATE module ${CMAKE_DL_LIBS})
  o3os_strict_warnings(o3m_${name}_test)
  target_compile_options(o3m_${name}_test PRIVATE -Wno-double-promotion)
  add_dependencies(o3m_${name}_test o3m_${name}_host)
  target_compile_definitions(o3m_${name}_test PRIVATE O3M_${upper}_HOST_SO="$<TARGET_FILE:o3m_${name}_host>" O3M_${upper}_TMOD="${CMAKE_CURRENT_BINARY_DIR}/${name}.tmod")
  if(TARGET o3m_${name}_tmod)
    add_dependencies(o3m_${name}_test o3m_${name}_tmod)  # the packed file, when the cross compiler is there: the loader's checks run on it
  endif()
  add_test(NAME o3m_${name}_test COMMAND o3m_${name}_test)
endfunction()

o3m_add_module(sine SOURCES sine/sine.cpp)
# The TB-303 (Open303 port): the hot loop at -O2 (the packer's default is -Os). The three built-in modules (below) are version 1.0.0: a card copy replaces one only with a higher version.
o3m_add_module(tb303 SOURCES tb303/tb303.cpp OPT -O2 VERSION 1.0.0)
o3m_manifest(tb303 tb303/module.json)
# OS-012c: the chip engine (written from scratch: pulse / triangle / LFSR noise, 4-bit steps, arpeggio, sweep, vibrato). The oscillator loops at -O2.
o3m_add_module(chip SOURCES chip/chip.cpp OPT -O2 VERSION 1.0.0)
o3m_manifest(chip chip/module.json)
o3m_add_test(chip chip/tests/chip_test.cpp)

# OS-SID: the SID-style voice (written from scratch: saw / triangle / pulse with PWM, 23-bit noise, combined waves, sync and ring, the C64's stepped ADSR; the filter is the next slice). The loops at -O2.
o3m_add_module(sid SOURCES sid/sid.cpp OPT -O2 VERSION 1.0.0)
o3m_manifest(sid sid/module.json)
o3m_add_test(sid sid/tests/sid_test.cpp)

# OS-012c: the FM engine (four operators, eight algorithms, written from scratch). The audio loop at -O2.
o3m_add_module(fm SOURCES fm/fm.cpp OPT -O2 VERSION 1.0.0)
o3m_manifest(fm fm/module.json)
o3m_add_test(fm fm/tests/fm_test.cpp)

# OS-012d: the built-in modules. The packed .tmod files of the TB-303, chip, FM and SID go into a generated table (tools/o3m_embed.py: the bytes as const arrays, flash on the MK66) that
# the images link (o3m_builtin) and hand to module::ModuleSet::set_builtins; module/builtin.h says how they meet the card's copies. The notices the code in them requires
# (builtin/NOTICES.txt and Open303's licence text) are in the same file as a string. Without the cross compiler there are no .tmod files: the table is empty and the tests that need it say so.
set(O3M_BUILTIN_STEMS tb303 chip fm sid)
set(O3M_BUILTIN_CPP "${CMAKE_CURRENT_BINARY_DIR}/builtin_modules.cpp")
set(O3M_BUILTIN_NOTICES builtin/NOTICES.txt tb303/LICENSE-Open303.txt)
set(_embed_args "")
set(_embed_deps "")
if(O3M_ARM_CXX)
  foreach(stem IN LISTS O3M_BUILTIN_STEMS)
    list(APPEND _embed_args "${stem}=${CMAKE_CURRENT_BINARY_DIR}/${stem}.tmod")
    list(APPEND _embed_deps "${CMAKE_CURRENT_BINARY_DIR}/${stem}.tmod")
  endforeach()
endif()
set(_notice_args "")
foreach(n IN LISTS O3M_BUILTIN_NOTICES)
  list(APPEND _notice_args --notice "${CMAKE_CURRENT_SOURCE_DIR}/${n}")
  list(APPEND _embed_deps "${CMAKE_CURRENT_SOURCE_DIR}/${n}")
endforeach()
add_custom_command(OUTPUT "${O3M_BUILTIN_CPP}"
  COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/o3m_embed.py --out "${O3M_BUILTIN_CPP}" ${_notice_args} ${_embed_args}
  DEPENDS ${_embed_deps} ${PROJECT_SOURCE_DIR}/tools/o3m_embed.py
  COMMENT "o3m_embed builtin_modules.cpp"
  VERBATIM)
add_library(o3m_builtin STATIC "${O3M_BUILTIN_CPP}")
target_link_libraries(o3m_builtin PUBLIC module)
o3os_strict_warnings(o3m_builtin)
if(O3M_ARM_CXX)
  add_dependencies(o3m_builtin o3m_tb303_tmod o3m_chip_tmod o3m_fm_tmod o3m_sid_tmod)
endif()

# OS-012b: the example app (ABI 1.1): input and a view, under /modules/apps on the card.
o3m_add_module(pads SOURCES apps/pads/pads.cpp)

# OS-H1 (ADR 0014, ABI 1.3): MIDI Monitor, an app that holds the midi_in capability (the file's capability word is the CAPS here and the manifest's "capabilities": the verify test compares them).
# Host fake of the new tables: tests/o3m_test_support.h. New Community OS behaviour.
o3m_manifest(midimon apps/midimon/module.json GENERATE)
o3m_add_module(midimon SOURCES apps/midimon/midimon.cpp INCLUDE apps/common VERSION 1.0.0 CAPS midi_in GEN)
o3m_add_test(midimon apps/midimon/tests/midimon_test.cpp)

# OS-H2a (ADR 0014, ABI 1.4): Loudness VU, an app that holds the audio_tap capability (peak / RMS / clip meters of the master or the input). Host fake of the tap: tests/o3m_test_support.h.
# New Community OS behaviour.
o3m_manifest(loudness apps/loudness/module.json GENERATE)
o3m_add_module(loudness SOURCES apps/loudness/loudness.cpp INCLUDE apps/common VERSION 1.0.0 CAPS audio_tap GEN)
o3m_add_test(loudness apps/loudness/tests/loudness_test.cpp)

# OS-H2b (ADR 0014, ABI 1.4): the two other apps on the audio tap, both audio_tap, both written from scratch, both main-loop only. TUNER (tap_stream, own low-pass and decimation, an FFT
# autocorrelation pitch detector) and SCOPE SPECTRUM (tap_latest; a rising-edge trigger, a 1024-point FFT). They share apps/common/ (the FFT, the text helpers: headers, INCLUDE). New Community OS behaviour.
o3m_manifest(tuner apps/tuner/module.json GENERATE)
o3m_add_module(tuner SOURCES apps/tuner/tuner.cpp INCLUDE apps/common VERSION 1.0.0 CAPS audio_tap GEN)
o3m_add_test(tuner apps/tuner/tests/tuner_test.cpp)
o3m_manifest(scope apps/scope/module.json GENERATE)
o3m_add_module(scope SOURCES apps/scope/scope.cpp INCLUDE apps/common VERSION 1.0.0 CAPS audio_tap GEN)
o3m_add_test(scope apps/scope/tests/scope_test.cpp)

# OS-TAPCALC: Tap Calc, an app (ABI 1.3: read_project for the project's tempo, edit_project for set_tempo, and the io table's clock): tap tempo and a BPM -> note length / LFO rate calculator that
# can set the project's tempo, under /modules/apps on the card. The arithmetic is a header (apps/tapcalc/tapcalc_math.h) that the host test includes beside driving the module through the ABI
# on a fake of the host tables. New Community OS behaviour.
o3m_manifest(tapcalc apps/tapcalc/module.json GENERATE)
o3m_add_module(tapcalc SOURCES apps/tapcalc/tapcalc.cpp INCLUDE apps/common VERSION 1.0.0 CAPS read_project,edit_project GEN)
o3m_add_test(tapcalc apps/tapcalc/tests/tapcalc_test.cpp)

# OS-H3A (ADR 0014, ABI 1.5): Rhythm Gen, the first app that edits the project (read_project + edit_project): a polyrhythm generator for the Pattern's block, opened from Pattern F7 TOOLS. The arithmetic is a header
# (apps/rhythmgen/rhythmgen_math.h) that the host test includes beside driving the module through the ABI on a fake of the host tables. New Community OS behaviour.
o3m_manifest(rhythmgen apps/rhythmgen/module.json GENERATE)
o3m_add_module(rhythmgen SOURCES apps/rhythmgen/rhythmgen.cpp INCLUDE apps/common VERSION 1.0.0 CAPS read_project,edit_project,play_notes GEN)
o3m_add_test(rhythmgen apps/rhythmgen/tests/rhythmgen_test.cpp)
if(TARGET o3m_rhythmgen_test)
  target_link_libraries(o3m_rhythmgen_test PRIVATE model)  # the Fill dialog's Euclid (model/pattern_fill.h) is the oracle of the rule
endif()

# OS-H3B (ADR 0014, ABI 1.5 / 1.6): the four editing apps, all read_project + edit_project, all opened from Pattern F7 TOOLS. CHORDS and HUMANIZE use only the step entries of 1.5 (and so declare 1.5); MUTATE and MORPH use
# the instrument-parameter entries of 1.6. They share apps/common/o3m_edit.h (text, the seeded draw, the target block, the page). The arithmetic of each is a header beside it that its host test includes
# beside driving the module through the ABI on the fake host tables. New Community OS behaviour.
o3m_manifest(humanize apps/humanize/module.json GENERATE)
o3m_add_module(humanize SOURCES apps/humanize/humanize.cpp INCLUDE apps/common VERSION 1.0.0 CAPS read_project,edit_project GEN)
o3m_add_test(humanize apps/humanize/tests/humanize_test.cpp)
o3m_manifest(chords apps/chords/module.json GENERATE)
o3m_add_module(chords SOURCES apps/chords/chords.cpp INCLUDE apps/common VERSION 1.0.0 CAPS read_project,edit_project GEN)
o3m_add_test(chords apps/chords/tests/chords_test.cpp)
if(TARGET o3m_chords_test)
  target_link_libraries(o3m_chords_test PRIVATE seq)  # the OS's chord table (seq/fx.h kChordMask) is the oracle of the app's copy of the index map
endif()
o3m_manifest(mutate apps/mutate/module.json GENERATE)
o3m_add_module(mutate SOURCES apps/mutate/mutate.cpp INCLUDE apps/common VERSION 1.0.0 CAPS read_project,edit_project GEN)
o3m_add_test(mutate apps/mutate/tests/mutate_test.cpp)
o3m_manifest(morph apps/morph/module.json GENERATE)
o3m_add_module(morph SOURCES apps/morph/morph.cpp INCLUDE apps/common VERSION 1.0.0 CAPS read_project,edit_project GEN)
o3m_add_test(morph apps/morph/tests/morph_test.cpp)

# OS-H4 (ADR 0014, ABI 1.7): SMF EXPORT, read_project + export_files. It writes the pattern (or the F7 TOOLS block) as a Standard MIDI File into AppExport/smfexport/ through the OS's export calls (they
# never block and write nothing else). smfexport_math.h is the file's arithmetic, which its host test includes beside driving the module through the ABI on the fake host tables. New Community OS behaviour.
o3m_manifest(smfexport apps/smfexport/module.json GENERATE)
o3m_add_module(smfexport SOURCES apps/smfexport/smfexport.cpp INCLUDE apps/common VERSION 1.0.0 CAPS read_project,export_files GEN)
o3m_add_test(smfexport apps/smfexport/tests/smfexport_test.cpp)
if(TARGET o3m_smfexport_test)
  target_link_libraries(o3m_smfexport_test PRIVATE seq)  # the core's micromove_start (seq/fx.h) is the oracle of the app's copy
endif()

# OS-012b: the templates of the SDK (modules/templates, copied by `o3m_gen.py new`), built as they are: manifest -> generated header -> module -> .tmod, and checked back
# against the manifest by the generator's reader and by the loader (tests/module_gen_test.cpp).
foreach(kind instrument effect app)
  o3m_manifest(example_${kind} templates/${kind}/module.json GENERATE)
  o3m_add_module(example_${kind} SOURCES templates/${kind}/example_${kind}.cpp GEN)
endforeach()

if(NOT O3OS_CROSS)
  add_test(NAME o3m_pack_selftest COMMAND ${Python3_EXECUTABLE} -I ${O3M_PACK} --self-test)
  add_test(NAME o3m_gen_selftest COMMAND ${Python3_EXECUTABLE} -I ${O3M_GEN} --self-test)
  add_test(NAME o3m_gen_scaffold COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tests/o3m_gen_scaffold_test.py ${O3M_GEN} ${PROJECT_SOURCE_DIR}/modules/templates)
  if(O3M_ARM_CXX)
    set_tests_properties(o3m_gen_scaffold PROPERTIES ENVIRONMENT "O3M_CXX=${O3M_ARM_CXX}")
  endif()
endif()

o3os_label_tests()
