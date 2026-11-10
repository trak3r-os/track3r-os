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

# OS-FLASHDIET I1/I2: the importer layout tag (model::kImportLayoutTag) printed by a host-compiled one-liner. The packer and o3m_gen.py read
# the checked-in model/import_layout_tag.txt as their default; ctest o3m_layout_tag_pin below holds that file to the printed value.
if(NOT O3OS_CROSS)
  add_executable(o3m_layout_tag "${PROJECT_SOURCE_DIR}/tools/o3m_layout_tag.cpp")
  target_link_libraries(o3m_layout_tag PRIVATE model)
  o3os_strict_warnings(o3m_layout_tag)
  add_custom_command(OUTPUT "${O3M_GEN_DIR}/import_layout_tag.txt"
    COMMAND $<TARGET_FILE:o3m_layout_tag> > "${O3M_GEN_DIR}/import_layout_tag.txt"
    DEPENDS o3m_layout_tag COMMENT "printing the importer layout tag" VERBATIM)
  add_custom_target(o3m_layout_tag_txt ALL DEPENDS "${O3M_GEN_DIR}/import_layout_tag.txt")
  add_test(NAME o3m_layout_tag_pin
    COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/o3m_layout_tag_check.py "${O3M_GEN_DIR}/import_layout_tag.txt")
endif()
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
      DEPENDS ${abs_sources} ${hdr_deps} ${gen_deps} ${O3M_PACK} ${PROJECT_SOURCE_DIR}/model/import_layout_tag.txt ${O3M_SDK_DIR}/o3module.h ${O3M_SDK_DIR}/o3module.ld  # the tag file is word 44 of every packed importer: a tag change must repack
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
        DEPENDS ${abs_sources} ${hdr_deps} ${gen_deps} ${O3M_PACK} ${PROJECT_SOURCE_DIR}/model/import_layout_tag.txt ${O3M_SDK_DIR}/o3module.h ${O3M_SDK_DIR}/o3module.ld  # the tag file is word 44 of every packed importer: a tag change must repack
        COMMENT "o3m_pack ${name}.raw (oracle)"
        VERBATIM)
      add_custom_target(o3m_${name}_raw ALL DEPENDS "${raw}")
      if(ARG_GEN)
        add_dependencies(o3m_${name}_raw o3m_${name}_gen)
      endif()
    endif()
  endif()
endfunction()

# o3m_add_test(<name> <source>... [HOST <module>]): a host test executable of one module, o3m_<name>_test (ctest `o3m_<name>_test`, OS-012c): doctest, the module's host shared object loaded with dlopen
# (O3M_<NAME>_HOST_SO) and driven through the ABI, the views through module::ViewHost (modules/tests/o3m_test_support.h). Host only; the test references are in double precision on purpose.
# HOST names the module whose host shared object the test loads (default <name>): a second test of one module (chip_golden) loads the module's own .so.
function(o3m_add_test name)
  if(O3OS_CROSS)
    return()
  endif()
  cmake_parse_arguments(ARG "" "HOST" "" ${ARGN})
  if(NOT ARG_HOST)
    set(ARG_HOST ${name})
  endif()
  string(TOUPPER ${name} upper)
  add_executable(o3m_${name}_test ${ARG_UNPARSED_ARGUMENTS})
  target_include_directories(o3m_${name}_test SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party)  # <doctest/doctest.h>
  target_link_libraries(o3m_${name}_test PRIVATE module ${CMAKE_DL_LIBS})
  o3os_strict_warnings(o3m_${name}_test)
  target_compile_options(o3m_${name}_test PRIVATE -Wno-double-promotion)
  add_dependencies(o3m_${name}_test o3m_${ARG_HOST}_host)
  target_compile_definitions(o3m_${name}_test PRIVATE O3M_${upper}_HOST_SO="$<TARGET_FILE:o3m_${ARG_HOST}_host>" O3M_${upper}_TMOD="${CMAKE_CURRENT_BINARY_DIR}/${ARG_HOST}.tmod")
  if(TARGET o3m_${ARG_HOST}_tmod)
    add_dependencies(o3m_${name}_test o3m_${ARG_HOST}_tmod)  # the packed file, when the cross compiler is there: the loader's checks run on it
  endif()
  add_test(NAME o3m_${name}_test COMMAND o3m_${name}_test)
endfunction()

o3m_add_module(sine SOURCES sine/sine.cpp)
# The TB-303 (Open303 port): the hot loop at -O2 (the packer's default is -Os). The three built-in modules (below) are version 1.0.0: a card copy replaces one only with a higher version.
o3m_add_module(tb303 SOURCES tb303/tb303.cpp OPT -O2 VERSION 1.0.0)
o3m_manifest(tb303 tb303/module.json)
# OS-012c: the chip engine (written from scratch: pulse / triangle / LFSR noise, 4-bit steps, arpeggio, sweep, vibrato). The oscillator loops at -O2.
# OS-CECHIP C2: version 1.1.0 (a card copy of 1.0.0 no longer shadows the built-in module), waves 4..11 from a port of tracker-ce's CHIP engine (chip/chip_ce.h, MIT: chip/LICENSE-tracker-ce.txt).
o3m_add_module(chip SOURCES chip/chip.cpp OPT -O2 VERSION 1.1.0)
o3m_manifest(chip chip/module.json)
o3m_add_test(chip chip/tests/chip_test.cpp)
o3m_add_test(chip_golden chip/tests/chip_golden_test.cpp HOST chip)  # OS-CECHIP G1: the golden renders of today's voice, recorded before any change to it
o3m_add_test(chip_ce chip/tests/chip_ce_test.cpp HOST chip)  # OS-CECHIP C2: the parameters 20..29, waves 4..11, Chord, hostile values, the module against the original

# OS-SID: the SID-style voice (written from scratch: saw / triangle / pulse with PWM, 23-bit noise, combined waves, sync and ring, the C64's stepped ADSR; the filter is the next slice). The loops at -O2.
o3m_add_module(sid SOURCES sid/sid.cpp OPT -O2 VERSION 1.0.0)
o3m_manifest(sid sid/module.json)
o3m_add_test(sid sid/tests/sid_test.cpp)

# OS-012c: the FM engine (four operators, eight algorithms, written from scratch). The audio loop at -O2.
o3m_add_module(fm SOURCES fm/fm.cpp OPT -O2 VERSION 1.0.0)
o3m_manifest(fm fm/module.json)
o3m_add_test(fm fm/tests/fm_test.cpp)

# OS-INSFX S8: the first sample effect, a biquad filter with mix for a track insert (modules/effects/biquad). Not built in: a .tmod for /modules/effects/ on the card, so the image does not grow.
# The descriptor header is generated into the build tree; the module's own host test compares it with a double-precision reference.
o3m_manifest(biquad effects/biquad/module.json GENERATE)
o3m_add_module(biquad SOURCES effects/biquad/biquad.cpp OPT -O2 VERSION 1.0.0 GEN)
o3m_add_test(biquad effects/biquad/tests/biquad_test.cpp)
if(TARGET o3m_biquad_test)
  target_include_directories(o3m_biquad_test PRIVATE ${O3M_GEN_DIR})  # biquad_gen.h: the parameter ids and ranges
  add_dependencies(o3m_biquad_test o3m_biquad_gen)
endif()

# OS-INSFX-ABI2 A2: the stereo effect template (modules/templates/effect_stereo, ABI 1.12: "stereo": "widens", factory presets), built as it is: manifest -> generated header -> module -> .tmod, and checked back
# against the manifest by the generator's reader and by the loader (tests/module_gen_test.cpp). Not in the loop of the three kinds below: it is a fourth template, and a .tmod for /modules/effects/ on the card.
o3m_manifest(example_stereo templates/effect_stereo/module.json GENERATE)
o3m_add_module(example_stereo SOURCES templates/effect_stereo/example_stereo.cpp GEN)
# OS-INSFX-ABI2 A6: the sidechain effect template (modules/templates/effect_key, ABI 1.12: a "type": "key" parameter, "stereo": "capable"), a ducker, built the same way.
o3m_manifest(example_key templates/effect_key/module.json GENERATE)
o3m_add_module(example_key SOURCES templates/effect_key/example_key.cpp GEN)

# OS-INSFX-FX2 X4 + X5: AUTOPAN, a stereo auto-pan insert (an LFO on the side, mono-compatible, synced to the beat through get_clock; ABI 1.12, a WIDENS effect, factory presets) with the optional
# HEADPHONE mode (a far-ear delay and head-shadow low-pass; modules/effects/autopan). Same wiring as the FX1 inserts: a .tmod for /modules/effects/ on the card, not in the image; the descriptor
# header is generated into the build tree; the module's own host test compares it with a double-precision reference.
o3m_manifest(autopan effects/autopan/module.json GENERATE)
o3m_add_module(autopan SOURCES effects/autopan/autopan.cpp OPT -O2 VERSION 1.0.0 GEN)
o3m_add_test(autopan effects/autopan/tests/autopan_test.cpp)
if(TARGET o3m_autopan_test)
  target_include_directories(o3m_autopan_test PRIVATE ${O3M_GEN_DIR})  # autopan_gen.h: the parameter ids and ranges
  add_dependencies(o3m_autopan_test o3m_autopan_gen)
endif()

# OS-INSFX-FX1: DRIVE, module 1 of the first effect pack, a saturation insert (soft/asymmetric shape, drive, tone, DC blocker, mix, output) for a track (modules/effects/drive). Not built in:
# a .tmod for /modules/effects/ on the card, so the image does not grow. The descriptor header is generated into the build tree; the module's own host test compares it with a double-precision
# reference of the same maths.
o3m_manifest(drive effects/drive/module.json GENERATE)
o3m_add_module(drive SOURCES effects/drive/drive.cpp OPT -O2 VERSION 1.0.0 GEN)
o3m_add_test(drive effects/drive/tests/drive_test.cpp)
if(TARGET o3m_drive_test)
  target_include_directories(o3m_drive_test PRIVATE ${O3M_GEN_DIR})  # drive_gen.h: the parameter ids and ranges
  add_dependencies(o3m_drive_test o3m_drive_gen)
endif()

# OS-INSFX-FX1: CRUSH, module 2 of the first effect pack, a bit-depth and sample-rate reduction insert (a dead-zone quantiser of 1 to 16 bits, a sample-and-hold of a fractional factor, mix, output) for a
# track (modules/effects/crush). Same wiring as DRIVE: a .tmod for /modules/effects/ on the card, not in the image; the descriptor header is generated into the build tree; the module's own host test
# compares it with a double-precision reference.
o3m_manifest(crush effects/crush/module.json GENERATE)
o3m_add_module(crush SOURCES effects/crush/crush.cpp OPT -O2 VERSION 1.0.0 GEN)
o3m_add_test(crush effects/crush/tests/crush_test.cpp)
if(TARGET o3m_crush_test)
  target_include_directories(o3m_crush_test PRIVATE ${O3M_GEN_DIR})  # crush_gen.h: the parameter ids and ranges
  add_dependencies(o3m_crush_test o3m_crush_gen)
endif()

# OS-INSFX-FX1: FOLD, module 3 of the first effect pack, a wavefolder insert (a level-compensated triangle fold, a symmetry offset, a post tone low-pass, an always-on DC blocker, mix, output) for a track
# (modules/effects/fold). Same wiring as DRIVE and CRUSH: a .tmod for /modules/effects/ on the card, not in the image; the descriptor header is generated into the build tree; the module's own host
# test compares it with a double-precision reference.
o3m_manifest(fold effects/fold/module.json GENERATE)
o3m_add_module(fold SOURCES effects/fold/fold.cpp OPT -O2 VERSION 1.0.0 GEN)
o3m_add_test(fold effects/fold/tests/fold_test.cpp)
if(TARGET o3m_fold_test)
  target_include_directories(o3m_fold_test PRIVATE ${O3M_GEN_DIR})  # fold_gen.h: the parameter ids and ranges
  add_dependencies(o3m_fold_test o3m_fold_gen)
endif()

# OS-INSFX-FX1: ENV FILTER, of the first effect pack, an envelope-controlled state-variable filter insert for a track (modules/effects/envfilter). Same wiring as CRUSH: a .tmod for
# /modules/effects/ on the card, not in the image; the descriptor header is generated into the build tree; the module's own host test compares it with a double-precision reference.
o3m_manifest(envfilter effects/envfilter/module.json GENERATE)
o3m_add_module(envfilter SOURCES effects/envfilter/envfilter.cpp OPT -O2 VERSION 1.0.0 GEN)
o3m_add_test(envfilter effects/envfilter/tests/envfilter_test.cpp)
if(TARGET o3m_envfilter_test)
  target_include_directories(o3m_envfilter_test PRIVATE ${O3M_GEN_DIR})  # envfilter_gen.h: the parameter ids and ranges
  add_dependencies(o3m_envfilter_test o3m_envfilter_gen)
endif()

# OS-INSFX-FX1: PHASER, module 3 of the first effect pack, a phaser insert (4 to 6 first-order all-pass stages swept by a sine LFO, a bounded feedback, mix) for a track (modules/effects/phaser).
# Same wiring as DRIVE and CRUSH: a .tmod for /modules/effects/ on the card, not in the image; the descriptor header is generated into the build tree; the module's own host test compares it with a
# double-precision reference.
o3m_manifest(phaser effects/phaser/module.json GENERATE)
o3m_add_module(phaser SOURCES effects/phaser/phaser.cpp OPT -O2 VERSION 1.0.0 GEN)
o3m_add_test(phaser effects/phaser/tests/phaser_test.cpp)
if(TARGET o3m_phaser_test)
  target_include_directories(o3m_phaser_test PRIVATE ${O3M_GEN_DIR})  # phaser_gen.h: the parameter ids and ranges
  add_dependencies(o3m_phaser_test o3m_phaser_gen)
endif()

# OS-INSFX-FX1: COMP, module 3 of the first effect pack, a compressor insert with a TRANSIENT mode (a soft-knee feed-forward compressor, or a transient shaper from a fast and a slow envelope follower; make-up,
# mix) for a track (modules/effects/comp). Same wiring as DRIVE and CRUSH: a .tmod for /modules/effects/ on the card, not in the image; the descriptor header is generated into the build tree; the module's own
# host test compares it with a double-precision reference.
o3m_manifest(comp effects/comp/module.json GENERATE)
o3m_add_module(comp SOURCES effects/comp/comp.cpp OPT -O2 VERSION 1.1.0 GEN)
o3m_add_test(comp effects/comp/tests/comp_test.cpp)
if(TARGET o3m_comp_test)
  target_include_directories(o3m_comp_test PRIVATE ${O3M_GEN_DIR})  # comp_gen.h: the parameter ids and ranges
  add_dependencies(o3m_comp_test o3m_comp_gen)
endif()

# OS-INSFX-FX2: PING-PONG, the tempo-synced stereo echo of the second effect pack (a WIDENS insert of ABI 1.12: render_st, the clock through get_clock, factory presets; modules/effects/pingpong). Same wiring as
# DRIVE and CRUSH: a .tmod for /modules/effects/ on the card, not in the image; the descriptor header is generated into the build tree; the module's own host test compares it with a double-precision
# reference of the same maths.
o3m_manifest(pingpong effects/pingpong/module.json GENERATE)
o3m_add_module(pingpong SOURCES effects/pingpong/pingpong.cpp OPT -O2 VERSION 1.0.0 GEN)
o3m_add_test(pingpong effects/pingpong/tests/pingpong_test.cpp)
if(TARGET o3m_pingpong_test)
  target_include_directories(o3m_pingpong_test PRIVATE ${O3M_GEN_DIR})  # pingpong_gen.h: the parameter ids and ranges
  add_dependencies(o3m_pingpong_test o3m_pingpong_gen)
endif()

# OS-INSFX-FX2 X1: WIDE, module 1 of the stereo pack, a stereo widener insert (a high-passed short delay of the mid added to the side: mono-compatible; ABI 1.12, a WIDENS effect, factory presets) for a track
# (modules/effects/wide). Same wiring as the FX1 inserts: a .tmod for /modules/effects/ on the card, not in the image; the descriptor header is generated into the build tree; the module's own host
# test compares it with a double-precision reference.
o3m_manifest(wide effects/wide/module.json GENERATE)
o3m_add_module(wide SOURCES effects/wide/wide.cpp OPT -O2 VERSION 1.0.0 GEN)
o3m_add_test(wide effects/wide/tests/wide_test.cpp)
if(TARGET o3m_wide_test)
  target_include_directories(o3m_wide_test PRIVATE ${O3M_GEN_DIR})  # wide_gen.h: the parameter ids and ranges
  add_dependencies(o3m_wide_test o3m_wide_gen)
endif()

# OS-INSFX-FX2 X2: STEREO CHORUS, a WIDENS insert for a track (modules/effects/chorus): one delay line of the mid, two taps that swing in opposite directions (L and R), Period, Depth, Delay, Mix,
# four factory presets. Same wiring as DRIVE: a .tmod for /modules/effects/ on the card, not in the image; the descriptor header is generated into the build tree; the module's own host test compares it
# with a double-precision reference of the same maths.
o3m_manifest(chorus effects/chorus/module.json GENERATE)
o3m_add_module(chorus SOURCES effects/chorus/chorus.cpp OPT -O2 VERSION 1.0.0 GEN)
o3m_add_test(chorus effects/chorus/tests/chorus_test.cpp)
if(TARGET o3m_chorus_test)
  target_include_directories(o3m_chorus_test PRIVATE ${O3M_GEN_DIR})  # chorus_gen.h: the parameter ids and ranges
  add_dependencies(o3m_chorus_test o3m_chorus_gen)
endif()

# OS-012d: the built-in module table, TEST ONLY since OS-FLASHDIET-ROOM R3: no image links o3m_builtin any more (the TB-303, chip, FM and SID are card modules, modules/instruments/
# of the SD content zip, tools/release/make_starter.py), but the host tests and the QEMU suites still run the real .tmod bytes through it. The packed .tmod files go into a generated
# table (tools/o3m_embed.py: OS-FLASHDIET A3, each file's 64-byte header raw and the rest deflated by tools/deflate_pack.py, our own deterministic encoder -- never Python's zlib,
# whose bytes would depend on the build machine's library) that a test hands to module::ModuleSet::set_builtins; module/builtin.h says how they meet the card's copies. The notices
# (builtin/NOTICES.txt and the licence texts) are in the same file as a string; the card carries them next to the module files. Without the cross compiler there are no .tmod files:
# the table is empty and the tests that need it say so.
set(O3M_BUILTIN_STEMS tb303 chip fm sid)
set(O3M_BUILTIN_CPP "${CMAKE_CURRENT_BINARY_DIR}/builtin_modules.cpp")
set(O3M_BUILTIN_NOTICES builtin/NOTICES.txt tb303/LICENSE-Open303.txt chip/LICENSE-tracker-ce.txt)
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
  DEPENDS ${_embed_deps} ${PROJECT_SOURCE_DIR}/tools/o3m_embed.py ${PROJECT_SOURCE_DIR}/tools/deflate_pack.py
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

# OS-FLASHDIET I1: the MOD / S3M / MED importers as one importer module, tracker.tmod (kind 3, ABI 1.11), for modules/importers/ on the card.
# Built from the formats_tracker library's sources (formats/mod, formats/med, formats/src/volume_level.cpp and the model and core sources
# they call) plus formats/src/tracker_sniff.cpp (the probe side, which the reader calls) and the module libc shim, so the packed image is
# the importers' real size and every packer refusal inside them surfaces here.
# The loader accepts the kind (the descriptor's importer table, the layout tag in header word 44) and holds it apart from the playable modules.
# OS-FLASHDIET I3: the image no longer links the importers itself; the module is the only place they run from (the starter zip ships it as
# modules/importers/tracker.tmod).
o3m_manifest(tracker importers/tracker/module.json)
# OS-FLASHDIET I1/I2: the packer writes model/import_layout_tag.txt (held to model::kImportLayoutTag by ctest o3m_layout_tag_pin) to word 44.
get_property(_tracker_sources GLOBAL PROPERTY O3OS_FORMATS_TRACKER_SOURCES)
set(_tracker_src_args "")
foreach(_src ${_tracker_sources})
  list(APPEND _tracker_src_args "../formats/${_src}")
endforeach()
o3m_add_module(tracker
  SOURCES importers/tracker/tracker.cpp importers/tracker/libc_shim.cpp ${_tracker_src_args} ../formats/src/tracker_sniff.cpp
          ../model/src/project.cpp ../model/src/project_state.cpp ../model/src/params.cpp ../model/src/settings.cpp ../model/src/instrument.cpp
          ../core/src/learn.cpp ../core/src/param.cpp ../core/src/fast_math.cpp
  INCLUDE ../formats/mod/include ../formats/med/include ../formats/include ../model/include ../core/include)

if(NOT O3OS_CROSS)
  add_test(NAME o3m_pack_selftest COMMAND ${Python3_EXECUTABLE} -I ${O3M_PACK} --self-test)
  add_test(NAME o3m_gen_selftest COMMAND ${Python3_EXECUTABLE} -I ${O3M_GEN} --self-test)
  add_test(NAME o3m_gen_scaffold COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tests/o3m_gen_scaffold_test.py ${O3M_GEN} ${PROJECT_SOURCE_DIR}/modules/templates)
  if(O3M_ARM_CXX)
    set_tests_properties(o3m_gen_scaffold PROPERTIES ENVIRONMENT "O3M_CXX=${O3M_ARM_CXX}")
  endif()
endif()

add_subdirectory(ptfx)  # the .ptfx plugins for the SD card's Effects/ folder (OS-PTFXPACK); a build product, not part of any image

o3os_label_tests()
