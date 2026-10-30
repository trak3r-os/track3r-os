# SPDX-License-Identifier: GPL-3.0-or-later

# ---- Target suites (OS-007a): O3OS_PLATFORM=qemu cross-builds these for the Cortex-M4F and runs them on QEMU
# (ctest `qemu_<name>`). One line per library: its test files (the same *_test.cpp the host build runs) and the
# libraries under test. A file that needs a host-only facility (threads, the file system, the simulator) is left
# out of its library's line; the host run still covers it. Never trim a test to fit: split the suite instead.
#   o3os_target_suite(<name> SOURCES <file>... DEPENDS <library>... [TIMEOUT <s>])
if(O3OS_PLATFORM STREQUAL "qemu")
  # The host fakes (std::deque, std::vector) are test doubles, not part of the target OS; the eve builder test needs
  # FakeDisplay, so its one source file is cross-built here for the suites that use it.
  add_library(qemu_host_fakes OBJECT ${PROJECT_SOURCE_DIR}/platform/host/src/fakes.cpp)
  target_include_directories(qemu_host_fakes PUBLIC ${PROJECT_SOURCE_DIR}/platform/host/include)
  target_link_libraries(qemu_host_fakes PUBLIC hal)
  o3os_strict_warnings(qemu_host_fakes)

  o3os_target_suite(core DEPENDS core
    SOURCES binding_resolver_test.cpp fixed_string_test.cpp fixed_vector_test.cpp gesture_test.cpp
            input_event_test.cpp keymap_test.cpp notify_test.cpp power_hold_test.cpp tick_test.cpp)   # ring_test.cpp: needs std::thread (host, TSan)
  # OS-002c: the pure parts of undo and jobs. jobs: the cooperative runner (budget, rotation, cancel). wav: every variant of the WAV
  # importer, truncation at every offset and a damage fuzz, no heap. model_undo: the journal, the Editor and its property test
  # against full snapshots (a 1.9 MB project in .bss: QEMU loads code + data + .bss into 4 MiB, so it is a suite of its own).
  o3os_target_suite(jobs DEPENDS core SOURCES job_test.cpp)
  o3os_target_suite(wav DEPENDS formats_wav SOURCES wav_test.cpp)
  # wav_sample: the importer as a registered format (the registry, the instrument a WAV makes, hostile files through the reader); one
  # project in .bss, like model_undo.
  o3os_target_suite(wav_sample DEPENDS formats SOURCES wav_sample_test.cpp)
  # OS-GRAN: wavetable formats on import: Serum's 'clm ' chunk, the frame rule, Surge's .wt (crafted files, truncation at every offset); one project in .bss, like wav_sample.
  o3os_target_suite(wavetable_import DEPENDS formats SOURCES wavetable_import_test.cpp)
  # OS-SAMPFMT: AIFF / AIFF-C and IFF 8SVX as a registered sample format (the 80-bit rate, every layout, refusals, truncation at every offset, a damage fuzz); one project in .bss, like wav_sample.
  o3os_target_suite(iff_sample DEPENDS formats SOURCES iff_sample_test.cpp)
  o3os_target_suite(model_undo DEPENDS model SOURCES undo_test.cpp TIMEOUT 300)
  o3os_target_suite(eve DEPENDS eve qemu_host_fakes SOURCES dl_test.cpp display_list_builder_test.cpp)
  # usb: the descriptor, class-request, ring, kernel and stream tests, including the M4 PKHBT/PKHTB/QADD16 paths, a
  # CCR.UNALIGN_TRP run of the kernels and a 40,000-SOF closed-loop run at both reference clocks. Left out:
  # usb_ring_thread_test.cpp (std::thread: host and TSan) and usb_sim_test.cpp (millions of SOFs: host only).
  o3os_target_suite(usb DEPENDS usb
    SOURCES usb_descriptor_test.cpp usb_uac2_test.cpp usb_kernels_test.cpp usb_ring_test.cpp usb_stream_test.cpp
    TIMEOUT 180)
  # OS-006b: the USB device logic on the Cortex-M4F, driven against the fake controller of tests/usb_fake_controller.h: the control pipe
  # (every standard request, ZLP and toggle sequences, stalls), the endpoint bookkeeping and the BDT layout (sizeof(BdtEntry) == 8 is only
  # checkable here), the interrupt (enumeration, CDC and MIDI streams, drop policies, halt, suspend, the loop bound), the diag line shell
  # and the diag log. Left out: usb_diag_log_thread_test.cpp (std::thread: host and TSan).
  o3os_target_suite(usb_device DEPENDS usb
    SOURCES usb_endpoint_test.cpp usb_control_test.cpp usb_device_test.cpp usb_cdc_test.cpp usb_diag_shell_test.cpp usb_diag_log_test.cpp
    TIMEOUT 180)
  # OS-005a: the audio engine skeleton's pure parts on the Cortex-M4F: the rate and interrupt-table contract, the slot protocol against
  # its reference model, the tone against the double oracle, AudioStatus and the diag text, the pipeline driven by hand. Left out:
  # audio_status_thread_test.cpp (std::thread: host and TSan). The fake-I2S suite (qemu_audio_fake) is in platform/qemu.
  o3os_target_suite(audio DEPENDS audio usb
    SOURCES audio_rate_test.cpp audio_slot_ring_test.cpp audio_test_signal_test.cpp audio_engine_test.cpp audio_status_test.cpp
            audio_pipeline_test.cpp
    TIMEOUT 120)
  # OS-005d lite: the sampler voices on the Cortex-M4F: the pitch increment and the interpolation against a double reference, and a note's exact sample
  # through the engine (a scripted event source, the real graph). Left out: audio_demo_test.cpp (the demo pattern end to end: a 1.9 MB project, host only).
  o3os_target_suite(audio_voices DEPENDS audio usb SOURCES audio_voices_test.cpp audio_voices_dir_test.cpp TIMEOUT 120)
  # OS-WAVEXP: the engine's offline render (render_offline against render_block, bit for bit; the park, the lease, what is put away and restored) with a scripted event source. The
  # sequencer's side (seq_offline_test.cpp: a project of 1.9 MB) and the file's (export_wav_test.cpp: an in-memory card) are host only.
  o3os_target_suite(audio_offline DEPENDS audio usb SOURCES audio_offline_test.cpp TIMEOUT 120)
  # OS-ODD11: the voice gate (a voice on an inaudible strip skips its post stages; the quiet mask, the re-entry ramp, the export untouched).
  o3os_target_suite(audio_gate DEPENDS audio usb SOURCES audio_gate_test.cpp TIMEOUT 120)
  # OS-PERFFX P0: the Perform layer in the voices and the strips: the performance filter against the instrument's, the layer's values across FxReset and Stopped, and an export that puts it away and gives it back (through the Engine).
  o3os_target_suite(audio_perform DEPENDS audio usb SOURCES audio_perform_test.cpp TIMEOUT 120)
  # OS-PERF-LFO (P4): the Perform rows VOL, PAN, TUNE, FLT and POS LFO against the instrument's own LFO at the offset index, the clamps, the layer's life across FxReset / Stopped / the export, the group 14's old rules.
  o3os_target_suite(audio_perflfo DEPENDS audio usb SOURCES audio_perflfo_test.cpp TIMEOUT 240)
  # OS-ODD11 slice 2: the closed-form advance of a gated voice (position, amplitude, envelope, end of a one shot) against the rendered voice, bit for bit, on the Cortex-M4F.
  o3os_target_suite(audio_advance DEPENDS audio usb SOURCES audio_advance_test.cpp TIMEOUT 240)
  # OS-CPU10 slice 2: the sampler voice's output pinned (one shots, loops, ramps, filters, modulators) while its loop, filter and the strip's pan law are rewritten: host and Cortex-M4F give one hash.
  o3os_target_suite(audio_voice_digest DEPENDS audio usb SOURCES audio_voice_digest_test.cpp TIMEOUT 240)
  # OS-CPU10 slice 2 (B1): the graph's output under scripted pan edits, mixed with the other strip edits and ramps in flight, pinned: the pan-only recompute gives the samples of the full one.
  o3os_target_suite(audio_graph_pan_digest DEPENDS audio usb SOURCES audio_graph_pan_digest_test.cpp TIMEOUT 240)
  # OS-LIVEP: a ParamChange of kind Instrument reaches the sampler voices' descriptors and the voices that play them (the Engine's instrument hook), the gain glide, the pitch, the filter, the pan.
  o3os_target_suite(audio_inst_live DEPENDS audio usb SOURCES audio_inst_live_test.cpp TIMEOUT 120)
  # OS-COREFX CF1: the tracker FX processor (arithmetic, then arpeggio, slides and fine portamento heard in a voice's block).
  o3os_target_suite(audio_tracker_fx DEPENDS audio usb SOURCES audio_tracker_fx_test.cpp TIMEOUT 180)
  # OS-007l: the instrument filter (svf.h) against a port of the Teensy fixed-point state variable filter, the bypass, stability at the top resonance, the voices and the Cutoff FX.
  o3os_target_suite(audio_svf DEPENDS audio usb SOURCES audio_svf_test.cpp TIMEOUT 180)
  # OS-005h: the Amiga voices (the pool, its events and filters, the engine path) and the sampler's bit-reduction engines on the Cortex-M4F. The Paula model against pt2-clone
  # (paula_oracle_test.cpp) and the MED tick processor against libxmp's frames (med_tick_test.cpp, which reads a file) are host only.
  o3os_target_suite(audio_amiga DEPENDS audio usb SOURCES amiga_voices_test.cpp TIMEOUT 180)
  # OS-007m: the filter's cutoff ADSR and LFO against double oracles (stage times and levels, shapes at known phases, the period against the tempo, the lock to the
  # transport, the Cutoff FX base, span independence, sanitising) and through the Engine (FxReset, Step, the event source's tempo).
  o3os_target_suite(audio_filter_mod DEPENDS audio usb SOURCES audio_filter_mod_test.cpp TIMEOUT 180)
  # OS-MOD: the pan (autopan), finetune (vibrato) and amp LFO (tremolo) modulators against double oracles, span independence, the live enable and disable, and the live loop points.
  o3os_target_suite(audio_mod DEPENDS audio usb SOURCES audio_mod_test.cpp TIMEOUT 180)
  # OS-GRAN: the granular voice against real-valued oracles (geometry, the three loop types, the shapes), the guard against a read outside the sample, span independence.
  o3os_target_suite(audio_granular DEPENDS audio usb SOURCES audio_granular_test.cpp TIMEOUT 600)  # ~137 s of wall time on an idle host (the 1,080-run guard test)
  # OS-GRAN: the wavetable's windows (the live window, no crossfade), the live granular position, and (item 3) the WT POS and GRAN POS modulators.
  o3os_target_suite(audio_wavetable DEPENDS audio usb SOURCES audio_wavetable_test.cpp TIMEOUT 180)
  # OS-005b: the fixed graph on the Cortex-M4F. audio_kernels: every sample kernel against its scalar reference and against the model of the
  # instructions (SMLAWB, SMLALD, SSUB16 + SEL, SHADD16, PKHBT, SSAT: the real ones run here), with CCR.UNALIGN_TRP set. audio_graph: the routing
  # truth table, the pan law and level curve, ramps, the reserved points, OUT MODE, the input strip and its latency, the meters and the taps.
  # Left out: audio_taps_thread_test.cpp (std::thread: host and TSan).
  o3os_target_suite(audio_kernels DEPENDS audio usb SOURCES audio_kernels_test.cpp TIMEOUT 300)
  o3os_target_suite(audio_graph DEPENDS audio usb SOURCES audio_graph_test.cpp audio_meters_test.cpp audio_taps_test.cpp audio_app_tap_test.cpp audio_usb_return_test.cpp audio_metro_test.cpp TIMEOUT 300)
  # OS-LIMIT: the master limiter on the Cortex-M4F: the ceiling and the transparency in single-precision hardware float (VFMA, VCVT, the flush of nothing: no denormal can arise), against the double model, and the limiter
  # in the graph (the chain's slot, the ParamChanges, the MAIN meters before it). The brute-force model's arrays are static (about 0.5 MB of .bss).
  o3os_target_suite(audio_limiter DEPENDS audio usb SOURCES audio_master_limiter_test.cpp audio_master_limiter_graph_test.cpp audio_scratch_test.cpp TIMEOUT 300)
  # OS-SENDFX: the two send effects on the Cortex-M4F with small static memory (VCVT + SSAT write, fused multiplies, the reverb's word-wise staging). The full property suite (the 4 s ring, every
  # corner) is audio_send_fx_test.cpp on the host.
  o3os_target_suite(audio_send_fx DEPENDS audio usb SOURCES audio_send_fx_m4_test.cpp TIMEOUT 300)
  # OS-007b: the MK66 platform's pure parts (memory map, crash record, clock and SDRAM register values, memory tests, LED codes,
  # interrupt map); the tests live beside the code in platform/mk66/tests and also run in the host build.
  # OS-002c adds one test that sees both the memory map and the undo journal (the layers may not include each other, so it lives here,
  # in tests/: the journal's region is the map's), run in the same suite. The same goes for OS-006b's console test (the real shell, pump and log
  # pager against the USB device and its fake controller, which live in tests/ and usb).
  # TIMEOUT 180: the suite runs about 233 s of guest time (icount), 41 s of wall time on an idle host; the default 60 s timed out twice under
  # a load of 35 on 20 cores (2026-10-12, two parallel builds), while the suite itself passed 316/316.
  # OS-012d: the built-in modules (the TB-303, chip and FM .tmod bytes in the image) loaded with no card by the loader into RAM that the Cortex-M4F executes, and a note played
  # through audio::ModuleVoices on each: the only place the module code runs in a test. The card's side of the rule (newer wins, older or broken ignored) is host only.
  o3os_target_suite(module_builtin DEPENDS module audio o3m_builtin SOURCES module_builtin_run_test.cpp TIMEOUT 120)
  # OS-MODSILENT: the same modules through the whole chain, as the image wires them: static module voices, the state arena, audition() on track 16, then the Engine's graph (the track strip,
  # MAIN, the TX block) must carry the sound. module_builtin covers the voices alone; this is the voice -> output block -> strip -> MAIN path that the unit's `mod note` takes.
  o3os_target_suite(module_chain DEPENDS module audio usb model seq midi o3m_builtin SOURCES module_chain_test.cpp TIMEOUT 120)
  # OS-GOV: the CPU governor of the module instruments: the admission rules, the cost estimator, the safety net, and twelve notes at once through the engine (native modules that cost what they
  # are told, and the real TB-303 with its cost simulated: the QEMU cycle counter says nothing about the unit).
  o3os_target_suite(module_governor DEPENDS module audio usb o3m_builtin SOURCES module_governor_test.cpp TIMEOUT 180)
  # The mk66 pure-part tests run here too, except the host-only ones: the screenshot service test writes to the host's DirCard (platform_host, std::filesystem), which the guest does not link.
  file(GLOB mk66_guest_tests CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/platform/mk66/tests/*_test.cpp)
  list(REMOVE_ITEM mk66_guest_tests ${PROJECT_SOURCE_DIR}/platform/mk66/tests/mk66_screenshot_service_test.cpp)
  o3os_target_suite(mk66 DEPENDS platform_mk66_pure model
    SOURCES ${mk66_guest_tests} mk66_undo_region_test.cpp mk66_usb_console_test.cpp TIMEOUT 180)

  # plot (OS-003d): the peak cache with its M4 SSUB16 + SEL kernels, the plot math and its libm-free log2 / exp2, the FFT and
  # the spectrum, the wave bitmap's rows and uploads, the curve reduction, the meters and the scope, the RAM_G map and the word
  # bound of every plot (measured with the host FakeDisplay, cross-built as for eve). Left out: what draws into the simulator's
  # rasteriser, which is megabytes of framebuffer and host only (plot_wave_test, plot_*_pixels_test, plot_raster_test,
  # plot_budget_test, plot_contrast_test).
  o3os_target_suite(plot DEPENDS ui eve qemu_host_fakes
    SOURCES plot_peak_cache_test.cpp plot_math_test.cpp plot_fft_test.cpp plot_ram_g_test.cpp plot_wave_bitmap_test.cpp
            plot_curve_test.cpp plot_meters_test.cpp plot_bounds_test.cpp
    TIMEOUT 300)
  # OS-003f: the boot screen's words, its flash budget and the hold; the pixels and the driver run (host only) in o3os_tests.
  o3os_target_suite(boot DEPENDS ui eve usb qemu_host_fakes SOURCES boot_screen_test.cpp)
  # OS-004b: the OG readers and writers, the settings chunks and the format registry. The native-format tests need
  # three 1.8 MiB arenas and the OG fixture survey reads files, so they stay on the host; these use two arenas
  # (3.8 MB of the 4 MiB of RAM) and cut their loops by stride on the target. What is left is about 230 KB of heap
  # for everything else, so the files are grouped three ways to keep the statics of each suite (a PAMD reader holds
  # 12.8 KB) and the peak of the heaviest test (the round trip of a whole folder) inside it.
  o3os_target_suite(formats_og_files DEPENDS formats
    SOURCES og_pattern_test.cpp og_instrument_test.cpp og_project_test.cpp og_settings_test.cpp)
  o3os_target_suite(formats_og_hostile DEPENDS formats TIMEOUT 300 SOURCES og_pamd_test.cpp og_hostile_test.cpp)
  o3os_target_suite(formats_og_registry DEPENDS formats SOURCES og_roundtrip_test.cpp format_registry_test.cpp)
  # OS-004c: project v17 (official 1.9.2) and the Export target; project files only, so it is a small suite of its own.
  o3os_target_suite(formats_og_v17 DEPENDS formats SOURCES og_v17_test.cpp)
  # OS-008a: the MED reader, the tempo conversion, the flow linearisation and their hostile-input loops (thinned by 40 on the
  # target). No model arena: a few KB of statics. Left out: med_oversize_test.cpp (a sample of 5.5 million frames, host only).
  o3os_target_suite(formats_med DEPENDS formats TIMEOUT 600
    SOURCES med_reader_test.cpp med_tempo_test.cpp med_flow_test.cpp med_hostile_test.cpp)
  # OS-008b: the M2 mapping (notes, volumes, the command table row by row, tracks, instruments) and the importer (golden songs, the PCM,
  # refuse / structural / truncate, every truncation and a mutation fuzz thinned for the target, the registered format). One project in
  # .bss, like wav_sample. Left out: med_import_big_test.cpp (megabytes of module: host only).
  o3os_target_suite(formats_med_import DEPENDS formats TIMEOUT 900
    SOURCES med_map_test.cpp med_import_test.cpp)
  # OS-MODIMP: the MOD / S3M reader (every layout, refusals, truncation at every offset and a mutation fuzz, thinned for the target) and the importer (golden songs, flow, tracks, instruments,
  # the three ways to end for lack of room, the registered formats); one project in .bss, like med_import. The fixture (support/modimp_fixture.h) is std::vector and std::map.
  o3os_target_suite(formats_modimp DEPENDS formats TIMEOUT 600 SOURCES modimp_reader_test.cpp)
  o3os_target_suite(formats_modimp_import DEPENDS formats TIMEOUT 900 SOURCES modimp_import_test.cpp)

  # OS-002d: parameters. params: the ids, the descriptor maths (curves, steps, display text and parsing it back), every row of the model's
  # tables, the settings block, the MIDI Learn rules, the ParamChange ring under load and the device settings file (no project). params_model:
  # the Project's parameters, a generation for each, undo and redo of them and the SETS / LRNB chunks of the native format: one project in
  # .bss (QEMU's 4 MiB hold code, data and .bss), like model_undo. Left out: params_app_test.cpp (the app, the simulator and the host fakes).
  o3os_target_suite(params DEPENDS formats SOURCES param_test.cpp settings_format_test.cpp TIMEOUT 300)
  o3os_target_suite(params_model DEPENDS formats SOURCES param_model_test.cpp TIMEOUT 600)
  # OS-USBRET: the device setting USB RETURN against the USB driver's flag (app/usb_return.h, a header): the decision of the main loop, against a fake driver and the real settings block.
  o3os_target_suite(usb_return DEPENDS model app SOURCES usb_return_sync_test.cpp)
  # OS-METRO: the device settings METRO and METRO VOL against the engine's and the sequencer's (app/metro_sync.h), against the real settings block and a ParamChange ring.
  o3os_target_suite(metro_sync DEPENDS model app seq SOURCES metro_sync_test.cpp)

  # OS-009 P1a.1: the edit rules of one step against the reference vectors (data/pattern_edit_vectors.h), the FX table, the scales
  # and the pad board. Pure: the model's rules and one small file of the app (the pad board), no project in .bss.
  o3os_target_suite(pattern_rules DEPENDS model app SOURCES pattern_edit_rules_test.cpp pad_board_test.cpp pattern_fill_test.cpp pattern_ops_test.cpp TIMEOUT 300)
  # OS-010a: the folder browser's listing (sorted, fixed capacity, letter groups, hostile names). Pure, no storage: the scan over a fake card (dir_scan_test.cpp)
  # and the File screen on the App (file_screen_test.cpp) are host only (the fake keeps std::string).
  # OS-FILES part B: the RECENT list and its file (app/recent_list.h): pure too, the round trip and a sweep of damage over the loader.
  o3os_target_suite(dir_index DEPENDS app SOURCES dir_index_test.cpp recent_file_test.cpp)

  # OS-005c: the MIDI timing core and the sequencer on the Cortex-M4F. midi: Q32 time, the messages, the timed queue, the hang book, the DIN emitter (fuzzed streams, the
  # lateness bounds), USB out, the router, MIDI in and the clock-follow loop against simulated masters. seq: the FX table and the control layers (no project). seq_model:
  # everything that reads a project (one in .bss, shared by the files through tests/support/seq_fixture.h): rows with the generation check, the sequencer's behaviour, the
  # reference-vector oracle, determinism over block sizes with the golden dumps, and the sequencer following an external clock. Left out: nothing; the loops are thinned for the target.
  o3os_target_suite(midi DEPENDS midi core
    SOURCES qtime_test.cpp midi_core_test.cpp midi_din_test.cpp midi_usb_test.cpp midi_router_test.cpp midi_in_test.cpp midi_cc_in_test.cpp midi_clock_dll_test.cpp midi_io_test.cpp
    TIMEOUT 600)
  o3os_target_suite(seq DEPENDS seq midi core SOURCES seq_fx_test.cpp seq_control_test.cpp TIMEOUT 300)
  o3os_target_suite(seq_model DEPENDS seq midi model
    SOURCES seq_rows_test.cpp seq_sequencer_test.cpp seq_follow_test.cpp seq_oracle_test.cpp seq_determinism_test.cpp seq_midi_link_test.cpp seq_pfx_test.cpp seq_metro_test.cpp seq_corefx_test.cpp seq_patswitch_test.cpp
    TIMEOUT 1800)

  o3os_label_tests()
  return()  # nothing below builds for the target: it is the host's unit test binary
endif()

# Unit tests (doctest, host only). Every *_test.cpp in this directory is picked up; 001b's ui/app tests
# need no change here because their libraries are linked when the targets exist.
find_package(Threads REQUIRED)

file(GLOB test_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
# Fixtures, fakes and chunk helpers shared with the round-trip tool below (support/ is not globbed).
set(support_sources ${CMAKE_CURRENT_SOURCE_DIR}/support/project_fixture.cpp)
# The Open303 oracle (OS-012a) only in the test binary, which links open303_oracle; o3pj_tool shares support_sources and does not.
set(oracle_sources ${CMAKE_CURRENT_SOURCE_DIR}/support/open303_ref.cpp)
# OS-007b: the MK66 platform's pure-part tests live beside the code (and also run on QEMU, see above; the screenshot service test is host only).
file(GLOB mk66_test_sources CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/platform/mk66/tests/*_test.cpp)
list(APPEND test_sources ${mk66_test_sources})
# The oracle wrapper includes the original's 2009 headers (double arithmetic, implicit conversions everywhere): no warnings for that one file.
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/support/open303_ref.cpp PROPERTIES COMPILE_FLAGS "-w")
add_executable(o3os_tests ${test_sources} ${support_sources} ${oracle_sources})
target_include_directories(o3os_tests SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party)  # <doctest/doctest.h>
target_link_libraries(o3os_tests PRIVATE
  core usb hal eve model formats platform_host midi seq module o3m_builtin
  $<$<TARGET_EXISTS:ui>:ui>
  $<$<TARGET_EXISTS:app>:app>
  $<$<TARGET_EXISTS:sim>:sim>
  $<$<TARGET_EXISTS:platform_mk66_pure>:platform_mk66_pure>
  $<$<TARGET_EXISTS:eve_riedel_oracle>:eve_riedel_oracle>
  Threads::Threads)
o3os_strict_warnings(o3os_tests)
target_link_libraries(o3os_tests PRIVATE formats_wav)  # OS-002c: the WAV importer
# OS-012a: the modules the tests load. The packer's files (module_fixture_test.cpp) exist when the cross compiler was found; the host shared objects always.
add_dependencies(o3os_tests o3m_sine_host o3m_tb303_host o3m_fm_host o3m_pads_host o3m_rhythmgen_host o3m_chords_host o3m_humanize_host o3m_mutate_host o3m_morph_host o3m_smfexport_host)
target_compile_definitions(o3os_tests PRIVATE O3M_SINE_HOST_SO="$<TARGET_FILE:o3m_sine_host>" O3M_TB303_HOST_SO="$<TARGET_FILE:o3m_tb303_host>" O3M_FM_HOST_SO="$<TARGET_FILE:o3m_fm_host>"
                           O3M_PADS_HOST_SO="$<TARGET_FILE:o3m_pads_host>" O3M_RHYTHMGEN_HOST_SO="$<TARGET_FILE:o3m_rhythmgen_host>"
                           O3M_CHORDS_HOST_SO="$<TARGET_FILE:o3m_chords_host>" O3M_HUMANIZE_HOST_SO="$<TARGET_FILE:o3m_humanize_host>"
                           O3M_MUTATE_HOST_SO="$<TARGET_FILE:o3m_mutate_host>" O3M_MORPH_HOST_SO="$<TARGET_FILE:o3m_morph_host>"
                           O3M_SMFEXPORT_HOST_SO="$<TARGET_FILE:o3m_smfexport_host>"
                           O3M_TB303_DIR="${PROJECT_SOURCE_DIR}/modules/tb303")
target_link_libraries(o3os_tests PRIVATE open303_oracle pt2_paula_oracle)
target_compile_definitions(o3os_tests PRIVATE O3_MED_TICK_GOLDEN="${CMAKE_CURRENT_SOURCE_DIR}/data/med_tick_golden.txt")
if(TARGET o3m_sine_tmod)
  add_dependencies(o3os_tests o3m_sine_tmod o3m_sine_raw o3m_tb303_tmod o3m_tb303_raw o3m_pads_tmod o3m_example_instrument_tmod o3m_example_effect_tmod o3m_example_app_tmod o3m_tb303_gen o3m_example_instrument_gen o3m_example_effect_gen o3m_example_app_gen)
  target_compile_definitions(o3os_tests PRIVATE O3M_FIXTURE_DIR="${PROJECT_BINARY_DIR}/modules" O3M_GEN_OUT_DIR="${PROJECT_BINARY_DIR}/modules/gen")
else()
  # No cross compiler, no fixtures: these files compile their no-fixture path (they skip, or fail under O3_CI=1), whose helpers are then unused.
  set_source_files_properties(module_fixture_test.cpp module_gen_test.cpp pads_app_test.cpp PROPERTIES COMPILE_OPTIONS "-Wno-unused-function;-Wno-unused-parameter")
endif()
target_link_libraries(o3os_tests PRIVATE ${CMAKE_DL_LIBS})

# GCC 13's ThreadSanitizer runtime aborts with "unexpected memory mapping" on kernels with high ASLR
# entropy (seen on Linux 6.8). Running the process with address randomisation off (setarch -R) avoids it.
set(runner "")
if(O3OS_TSAN)
  find_program(SETARCH_EXECUTABLE setarch)
  if(SETARCH_EXECUTABLE)
    set(runner ${SETARCH_EXECUTABLE} ${CMAKE_HOST_SYSTEM_PROCESSOR} -R)
  endif()
endif()

# The unit binary runs as several ctests (OS-CI), not one: a single run took 157 s under ASan + UBSan (one timeout at 300 s), and a
# CI job wants the slices in parallel, each under its own timeout. A group is a list of doctest source-file patterns
# (--source-file; `*` matches any run of characters, `/` included, so each starts with `*/`); `unit_rest` is every case no group
# names, so a new *_test.cpp joins it by itself. Wall time with ASan + UBSan, 20 cores free: fuzz 36 s (the native-format fuzz loops
# of OS-004a), usb 36 s (the OS-006a closed-loop SOF simulations), formats 30 s, model 21 s (the undo property test), plot 20 s; the rest
# (OS-CIFAST: it took 105 s in one piece) split by area, audio ~30 s, ui ~40 s (overlay, the budget harness, the pattern view), seq ~15 s,
# what is left ~25 s. The areas are also the ctest labels (cmake/O3osLabels.cmake): the groups are what `ci.sh` scopes by.
# unit_cover asserts that doctest's --count of the groups adds up to the whole binary's and that none is empty: no case runs twice or
# not at all (below, with the other Python tests). The named verdict ctests after the groups, contrast, budget, plot ..., deliberately
# re-run cases of these. The timeout is a guard against a hang, not the budget: with four such tests at once on four cores the longest
# (fuzz) took 80 s.
set(o3os_unit_groups fuzz usb model formats plot audio seq ui apps)
set(o3os_unit_fuzz */native_fuzz_test.cpp)
set(o3os_unit_usb */usb_*_test.cpp)
set(o3os_unit_model */undo_test.cpp */undo_area_test.cpp */model_test.cpp */param*_test.cpp */settings_format_test.cpp)
set(o3os_unit_formats */native_format_test.cpp */og_*_test.cpp */med_*_test.cpp */modimp_*_test.cpp */wav*_test.cpp */iff_sample_test.cpp */format_registry_test.cpp
                      */import_job_test.cpp */project_state_test.cpp */persist2_format_test.cpp */law_format_test.cpp */appm_format_test.cpp */lfosine_format_test.cpp)
set(o3os_unit_plot */plot_*_test.cpp */raster*_test.cpp)
set(o3os_unit_audio */audio_*_test.cpp */tb303_*_test.cpp */paula_oracle_test.cpp */amiga_*_test.cpp)
set(o3os_unit_seq */seq_*_test.cpp */midi_*_test.cpp */metro_*_test.cpp */play_*_test.cpp)
# OS-APPUX S2/S3: the shipped apps through the whole stack in the simulator (about a minute under the sanitizers: eleven apps, each driven key by key)
set(o3os_unit_apps */app_grammar_test.cpp */sim_apps_test.cpp)
set(o3os_unit_ui */overlay_test.cpp */budget_test.cpp */pattern_view_test.cpp */pattern_zoom_test.cpp */legibility_test.cpp)
# Every pattern of every group names a file: a pattern that matches none (a file renamed or removed) is a configure error, not a hole
# that unit_cover finds only if the whole group goes empty. (unit_rest is the exclusion of all of them: a new file lands there.)
foreach(group IN LISTS o3os_unit_groups)
  foreach(pattern IN LISTS o3os_unit_${group})
    string(REPLACE "*/" "" pattern_file "${pattern}")
    file(GLOB matched "${CMAKE_CURRENT_SOURCE_DIR}/${pattern_file}")
    if(NOT matched)
      message(FATAL_ERROR "unit group '${group}': the pattern '${pattern}' matches no test file in ${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
  endforeach()
endforeach()
set(o3os_unit_all "")
set(o3os_unit_cover_groups "")
foreach(group IN LISTS o3os_unit_groups)
  list(JOIN o3os_unit_${group} "," filter)
  list(APPEND o3os_unit_all ${o3os_unit_${group}})
  add_test(NAME unit_${group} COMMAND ${runner} $<TARGET_FILE:o3os_tests> "--source-file=${filter}")
  set_tests_properties(unit_${group} PROPERTIES TIMEOUT 240 FAIL_REGULAR_EXPRESSION "test cases: +0 ")
  list(APPEND o3os_unit_cover_groups "--source-file=${filter}")
endforeach()
list(JOIN o3os_unit_all "," filter)
add_test(NAME unit_rest COMMAND ${runner} $<TARGET_FILE:o3os_tests> "--source-file-exclude=${filter}")
set_tests_properties(unit_rest PROPERTIES TIMEOUT 600 FAIL_REGULAR_EXPRESSION "test cases: +0 ")
list(APPEND o3os_unit_cover_groups "--source-file-exclude=${filter}")

# Groups of the unit tests that gate the look (OS-003b) also run on their own, so the verdict is its own line in the
# ctest summary: the contrast of every token pair of every theme (ADR 0010 amendment 1), the display-list budget of
# every state in every theme and both translate modes (ADR 0010 section 9), and the fallback's pixels. FAIL_REGULAR_EXPRESSION guards against a renamed test case
# silently matching nothing.
add_test(NAME contrast COMMAND ${runner} $<TARGET_FILE:o3os_tests> "--test-case=theme contrast*")
set_tests_properties(contrast PROPERTIES TIMEOUT 120 FAIL_REGULAR_EXPRESSION "test cases: +0 ")
add_test(NAME budget COMMAND ${runner} $<TARGET_FILE:o3os_tests> "--test-case=budget:*")
set_tests_properties(budget PROPERTIES TIMEOUT 120 FAIL_REGULAR_EXPRESSION "test cases: +0 ")
# The VERTEX2F fallback (plan B if the chip does not translate VERTEX2II) must draw the translated path's picture.
add_test(NAME translate_fallback COMMAND ${runner} $<TARGET_FILE:o3os_tests> "--test-case=translate:*")
set_tests_properties(translate_fallback PROPERTIES TIMEOUT 120 FAIL_REGULAR_EXPRESSION "test cases: +0 ")

# OS-005a: the MK66 memory map as its own verdict: the SDRAM regions sum to 16 MiB with the stated slack, the project region holds the
# model's worst-case arena, the SRAM_L and SRAM_U budgets hold (ADR 0013 D7). The static_asserts in mem_map.h stop a build that breaks
# them; these cases pin the numbers.
add_test(NAME mk66_memmap COMMAND ${runner} $<TARGET_FILE:o3os_tests> "--test-case=mk66 mem_map*,mem_map x model*")
set_tests_properties(mk66_memmap PROPERTIES TIMEOUT 60 FAIL_REGULAR_EXPRESSION "test cases: +0 ")

# OS-003d, the plot library (ui/plot): its unit groups run on their own so that the verdict is its own line in the ctest
# summary: the kernels (peak cache, FFT, math), the drawing (wave, curve, spectrum, meters, scope, the renderer's raw
# sections) and the word budget of the plot gallery's states in every theme and both translate modes.
add_test(NAME plot COMMAND ${runner} $<TARGET_FILE:o3os_tests> "--test-case=plot *,renderer raw*" "--test-case-exclude=plot budget*")
set_tests_properties(plot PROPERTIES TIMEOUT 300 FAIL_REGULAR_EXPRESSION "test cases: +0 ")
add_test(NAME plot_budget COMMAND ${runner} $<TARGET_FILE:o3os_tests> "--test-case=plot budget*")
set_tests_properties(plot_budget PROPERTIES TIMEOUT 120 FAIL_REGULAR_EXPRESSION "test cases: +0 ")

# The plot library's host benches (relative figures; build with -DCMAKE_BUILD_TYPE=Release for numbers). ctest runs a smoke pass.
add_executable(o3os_plot_bench bench/plot_bench.cpp)
target_link_libraries(o3os_plot_bench PRIVATE ui eve platform_host)
o3os_strict_warnings(o3os_plot_bench)
add_test(NAME plot_bench_smoke COMMAND ${runner} $<TARGET_FILE:o3os_plot_bench> --smoke)
set_tests_properties(plot_bench_smoke PROPERTIES TIMEOUT 300)

# OS-005b: the audio graph's host benches (relative figures: the stages of one block, the paired kernels against their references).
add_executable(o3os_audio_bench bench/audio_bench.cpp)
target_link_libraries(o3os_audio_bench PRIVATE audio usb model core hal)
o3os_strict_warnings(o3os_audio_bench)
add_test(NAME audio_bench_smoke COMMAND ${runner} $<TARGET_FILE:o3os_audio_bench> --smoke)
set_tests_properties(audio_bench_smoke PROPERTIES TIMEOUT 300)

# OS-PFX: the sequencer's step() cost per block (idle, dense, dense with the Chord / Arp / Tune / Slide FX): relative figures, ctest runs a smoke pass.
add_executable(o3os_seq_bench bench/seq_bench.cpp)
target_link_libraries(o3os_seq_bench PRIVATE seq midi model core audio hal)
o3os_strict_warnings(o3os_seq_bench)
add_test(NAME seq_bench_smoke COMMAND ${runner} $<TARGET_FILE:o3os_seq_bench> --smoke)
set_tests_properties(seq_bench_smoke PROPERTIES TIMEOUT 300)

if(O3OS_TSAN)
  # The ring is the one lock-free structure shared between contexts: run its cases on their own under
  # TSan, labelled so `ctest -L tsan` selects them. FAIL_REGULAR_EXPRESSION guards against a renamed
  # test case silently matching nothing (the exit code still counts: a reported race exits non-zero).
  add_test(NAME ring_tsan COMMAND ${runner} $<TARGET_FILE:o3os_tests> --test-case=ring:*)
  set_tests_properties(ring_tsan PROPERTIES
    LABELS tsan
    TIMEOUT 120
    FAIL_REGULAR_EXPRESSION "test cases: +0 ")
  # The USB overwrite ring is the other lock-free structure with two contexts (its slots are relaxed atomics, so
  # a copy that races with an overwrite must be race-free for TSan and merely rejected by the validation).
  add_test(NAME usb_ring_tsan COMMAND ${runner} $<TARGET_FILE:o3os_tests> --test-case=usb_ring:*)
  set_tests_properties(usb_ring_tsan PROPERTIES
    LABELS tsan
    TIMEOUT 120
    FAIL_REGULAR_EXPRESSION "test cases: +0 ")
  # OS-005a: AudioStatus's render group is a seqlock between the render (one writer) and any reader: run its two-thread case alone.
  add_test(NAME audio_status_tsan COMMAND ${runner} $<TARGET_FILE:o3os_tests> "--test-case=audio status thread:*")
  set_tests_properties(audio_status_tsan PROPERTIES
    LABELS tsan
    TIMEOUT 120
    FAIL_REGULAR_EXPRESSION "test cases: +0 ")
  # OS-303PERF: a module instance's perf cell (a seqlock; add() has the render for its one writer) read by the main loop, against a concurrent writer.
  add_test(NAME module_perf_tsan COMMAND ${runner} $<TARGET_FILE:o3os_tests> "--test-case=module perf thread:*")
  set_tests_properties(module_perf_tsan PROPERTIES
    LABELS tsan
    TIMEOUT 120
    FAIL_REGULAR_EXPRESSION "test cases: +0 ")
  # OS-005b: the meter snapshot (a seqlock) and the sample rings' lap detection, each against a concurrent writer.
  add_test(NAME audio_taps_tsan COMMAND ${runner} $<TARGET_FILE:o3os_tests> "--test-case=audio taps thread:*")
  set_tests_properties(audio_taps_tsan PROPERTIES
    LABELS tsan
    TIMEOUT 120
    FAIL_REGULAR_EXPRESSION "test cases: +0 ")
  # OS-WAVEXP: the ownership protocol of the engine between the render line and the export (Engine::offline_*): two threads, the render one standing down on a request.
  # (The case's name has no comma: doctest splits a filter on it, and the case matched nothing. FAIL_REGULAR_EXPRESSION fails the test when no case ran.)
  add_test(NAME audio_offline_tsan COMMAND ${runner} $<TARGET_FILE:o3os_tests> "--test-case=offline engine threaded:*")
  set_tests_properties(audio_offline_tsan PROPERTIES
    LABELS tsan
    TIMEOUT 120
    FAIL_REGULAR_EXPRESSION "test cases: +0 ")
  # E-SONG-07: the order list's edit journal (Project::snapshot_order, a seqlock: the main loop is the one writer, the render the reader; its words are relaxed atomics) against a concurrent writer.
  add_test(NAME song_order_tsan COMMAND ${runner} $<TARGET_FILE:o3os_tests> "--test-case=song order thread:*")
  set_tests_properties(song_order_tsan PROPERTIES
    LABELS tsan
    TIMEOUT 120
    FAIL_REGULAR_EXPRESSION "test cases: +0 ")
endif()

# OS-004a: the native project format against its Python reference. o3pj_tool writes, loads and dumps the
# fixtures; tests/o3project_roundtrip.py runs C++ -> Python -> C++ over them and compares the dumps.
add_executable(o3pj_tool tools/o3pj_tool.cpp ${support_sources})
target_link_libraries(o3pj_tool PRIVATE model formats)
o3os_strict_warnings(o3pj_tool)
# OS-E2E J2: the OG project of the migration journey (the card e2e_og, sim/CMakeLists.txt target e2e_og_fixture), written from the synthetic byte maps of support/og_fixture.h.
add_executable(o3og_fixture tools/o3og_fixture.cpp ${support_sources})
target_link_libraries(o3og_fixture PRIVATE model formats)
o3os_strict_warnings(o3og_fixture)
# OS-QANET D1: a fixture folder into a sparse FAT32 image (the simulator's `card <image>`: the unit's FatFs over it). platform_host_fat is host only.
if(TARGET platform_host_fat)
  add_executable(o3fatimg tools/o3fatimg.cpp)
  target_link_libraries(o3fatimg PRIVATE platform_host_fat)
  o3os_strict_warnings(o3fatimg)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
# OS-CI: the unit groups above cover every case of the binary exactly once.
add_test(NAME unit_cover
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/unit_cover.py ${runner} $<TARGET_FILE:o3os_tests> -- ${o3os_unit_cover_groups})
set_tests_properties(unit_cover PROPERTIES TIMEOUT 60)
# OS-FINALQA FQA-7: the heavy song of the unit's CPU run (`o3pj_tool gen heavy`) against the mix of docs/architecture/cpu-budget.md section 4.
add_test(NAME heavy_song_python COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/heavy_song_test.py $<TARGET_FILE:o3pj_tool>)
set_tests_properties(heavy_song_python PROPERTIES TIMEOUT 60)
# OS-CLEANUP: the songs of the step-FX CPU runs (`o3pj_tool gen lfostepNN|lfostepoff|moddriveN[off]`) against the recipe the HIL scenarios qa_lfostep and qa_cpu_moddrive expect.
add_test(NAME hil_songs_python COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/hil_songs_test.py $<TARGET_FILE:o3pj_tool>)
set_tests_properties(hil_songs_python PROPERTIES TIMEOUT 120)
# OS-CLEANUP: the generated HIL scenarios are the committed ones (platform/mk66/tools/hil/gen_hil.py is the one place they are edited). Reads files only.
add_test(NAME hil_scenarios_sync COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/hil_scenarios_sync_test.py ${PROJECT_SOURCE_DIR})
set_tests_properties(hil_scenarios_sync PROPERTIES TIMEOUT 60)
add_test(NAME project_python
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/o3project_roundtrip.py
          $<TARGET_FILE:o3pj_tool> ${PROJECT_SOURCE_DIR}/tools/o3project.py ${CMAKE_CURRENT_BINARY_DIR}/project_python)
set_tests_properties(project_python PROPERTIES TIMEOUT 300)
# OS-E2E-1b: SimProject (sim/src/sim_project.cpp) copies the platform's load sequence (finish_load, begin_samples, begin_replace in platform/mk66/src/app_main.cpp, firmware: not shareable). Reads the
# two files only: the calls of the three functions must be in sim_project.cpp or on the test's "not simulated" list, so a hand-over step added on the unit cannot be forgotten in the simulator.
add_test(NAME sim_project_drift
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/sim_project_drift_test.py ${PROJECT_SOURCE_DIR})
set_tests_properties(sim_project_drift PROPERTIES TIMEOUT 60)
# OS-E2E-1b: the simulator's card fixture (<build>/sim/cards/e2e_card, the card of scenarios/e2e/e2e_card.txt) is generated at build time (sim/CMakeLists.txt, target e2e_card_fixture, never
# committed): the test holds it to what `o3pj_tool gen small` writes, and to nothing else on it.
get_property(O3SIM_CARDS_DIR GLOBAL PROPERTY O3SIM_CARDS_DIR)
if(O3SIM_CARDS_DIR)
  add_test(NAME e2e_card_fixture_fresh
    COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/e2e_card_fixture_test.py ${O3SIM_CARDS_DIR}/e2e_card $<TARGET_FILE:o3pj_tool>)
  set_tests_properties(e2e_card_fixture_fresh PROPERTIES TIMEOUT 60)
  # OS-E2E-13: the same for the pro-session journey's card (sim/CMakeLists.txt, target e2e_pro_fixture): the project and the four WAVs of `o3pj_tool gen kit`
  add_test(NAME e2e_pro_fixture_fresh
    COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/e2e_pro_fixture_test.py ${O3SIM_CARDS_DIR}/e2e_pro $<TARGET_FILE:o3pj_tool>)
  set_tests_properties(e2e_pro_fixture_fresh PROPERTIES TIMEOUT 60)
  # OS-E2E J2: and the OG project of the migration journey (target e2e_og_fixture): the three OG files of `o3og_fixture`
  add_test(NAME e2e_fixture_fresh
    COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/e2e_og_fixture_test.py ${O3SIM_CARDS_DIR}/e2e_og $<TARGET_FILE:o3og_fixture>)
  set_tests_properties(e2e_fixture_fresh PROPERTIES TIMEOUT 60)
  # sim_card_test.cpp copies the generated card (read only) into scratch directories of its own
  add_dependencies(o3os_tests e2e_card_fixture)
  target_compile_definitions(o3os_tests PRIVATE O3_E2E_CARD_DIR="${O3SIM_CARDS_DIR}/e2e_card")
  # OS-QANET: the random user on the large generated card (tests/qanet/CMakeLists.txt)
  add_subdirectory(qanet)
endif()

# OS-QANET C1: the differential audio matrix (tests/qanet/audio_matrix.cmake: its own binary and ctests `qanet_audio_matrix.<cell>`).
include(${CMAKE_CURRENT_SOURCE_DIR}/qanet/audio_matrix.cmake)

# OS-QANET A1: every bound action against the destructive-change rule (tests/qanet/, docs/specs/OS-QANET.md part A). Its own binary: it needs no other test's support and runs in one piece. It drives the
# simulator over a scratch copy of the generated card fixture (above), so it exists where the simulator and the fixture do.
if(TARGET sim AND O3SIM_CARDS_DIR)
  # qanet/rules.{h,cpp}: the rules as functions over a state before and after one action, shared with the other parts of the net (the random user of part B).
  add_library(o3os_qanet_rules_lib STATIC qanet/rules.cpp qanet/rig.cpp qanet/selection_ops.cpp)
  target_include_directories(o3os_qanet_rules_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/qanet)
  target_link_libraries(o3os_qanet_rules_lib PUBLIC app model sim platform_host)
  target_compile_definitions(o3os_qanet_rules_lib PRIVATE O3_E2E_CARD_DIR="${O3SIM_CARDS_DIR}/e2e_card")
  o3os_strict_warnings(o3os_qanet_rules_lib)
  add_executable(o3os_qanet_rules qanet/rules_test.cpp qanet/action_states.cpp)
  target_link_libraries(o3os_qanet_rules PRIVATE o3os_qanet_rules_lib)
  target_include_directories(o3os_qanet_rules SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party)  # <doctest/doctest.h>
  target_link_libraries(o3os_qanet_rules PRIVATE
    core usb hal eve model formats platform_host midi seq module o3m_builtin ui app sim formats_wav Threads::Threads ${CMAKE_DL_LIBS})
  o3os_strict_warnings(o3os_qanet_rules)
  add_dependencies(o3os_qanet_rules e2e_card_fixture)
  target_compile_definitions(o3os_qanet_rules PRIVATE
    O3_E2E_CARD_DIR="${O3SIM_CARDS_DIR}/e2e_card"
    O3_QANET_BASELINE="${CMAKE_CURRENT_SOURCE_DIR}/qanet/baseline_rules.txt"
    O3_QANET_ALLOW="${CMAKE_CURRENT_SOURCE_DIR}/qanet/allow_rules.txt"
    O3_QANET_GAPS="${CMAKE_CURRENT_SOURCE_DIR}/qanet/never_changed.txt")
  # The walk is one fresh simulator per binding (about 500 runs, 90 s under ASan + UBSan on one core): it runs as four tests, each over a quarter of the contexts (QANET_SHARD, rules_test.cpp), in
  # parallel. The self-tests of the rules (a planted fault, the baseline comparison, the table check) are a test of their own, with no walk, and so is the gap pass (never_changed.txt: the actions that no run saw
  # act; it needs every context in one pass, which a shard cannot give). `ctest -R qanet_rules` runs all six.
  foreach(shard 1 2 3 4)
    add_test(NAME qanet_rules_${shard}of4 COMMAND ${runner} $<TARGET_FILE:o3os_qanet_rules> "--test-case=qanet rules: every bound*")
    set_tests_properties(qanet_rules_${shard}of4 PROPERTIES TIMEOUT 600 ENVIRONMENT "QANET_SHARD=${shard}/4" FAIL_REGULAR_EXPRESSION "test cases: +0 ")
  endforeach()
  add_test(NAME qanet_rules_selftest COMMAND ${runner} $<TARGET_FILE:o3os_qanet_rules> "--test-case-exclude=qanet rules: every bound*,qanet rules gaps*")
  set_tests_properties(qanet_rules_selftest PROPERTIES TIMEOUT 300 FAIL_REGULAR_EXPRESSION "test cases: +0 ")
  add_test(NAME qanet_rules_gaps COMMAND ${runner} $<TARGET_FILE:o3os_qanet_rules> "--test-case=qanet rules gaps*")
  set_tests_properties(qanet_rules_gaps PROPERTIES TIMEOUT 600 FAIL_REGULAR_EXPRESSION "test cases: +0 ")

  # OS-QANET A2: every bound key of every state (tests/qanet/keys_test.cpp). Its own binary, the same libraries; six shards over the states and a self-test, as above.
  add_executable(o3os_qanet_keys qanet/keys_test.cpp qanet/action_states.cpp)
  target_link_libraries(o3os_qanet_keys PRIVATE o3os_qanet_rules_lib)
  target_include_directories(o3os_qanet_keys SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party)
  target_link_libraries(o3os_qanet_keys PRIVATE
    core usb hal eve model formats platform_host midi seq module o3m_builtin ui app sim formats_wav Threads::Threads ${CMAKE_DL_LIBS})
  o3os_strict_warnings(o3os_qanet_keys)
  add_dependencies(o3os_qanet_keys e2e_card_fixture)
  target_compile_definitions(o3os_qanet_keys PRIVATE
    O3_QANET_KEYS_BASELINE="${CMAKE_CURRENT_SOURCE_DIR}/qanet/baseline_keys.txt"
    O3_QANET_ALLOW="${CMAKE_CURRENT_SOURCE_DIR}/qanet/allow_rules.txt"
    O3_QANET_KEYS_GAPS="${CMAKE_CURRENT_SOURCE_DIR}/qanet/gaps_keys.txt")
  # Each shard writes the pattern lines of the baseline, allow and gaps files that it matched; qanet_keys_patterns runs after all six and fails on a pattern that none of them needed (a shard cannot tell).
  set(qanet_hits_dir ${CMAKE_CURRENT_BINARY_DIR}/qanet_keys_hits)
  set(qanet_hits_files "")
  foreach(shard 1 2 3 4 5 6)
    list(APPEND qanet_hits_files ${qanet_hits_dir}/hits_${shard}.txt)
  endforeach()
  # A stale file of an earlier run must not satisfy qanet_keys_patterns: a setup step deletes them before any shard runs.
  add_test(NAME qanet_keys_hits_clean COMMAND ${CMAKE_COMMAND} -E rm -f ${qanet_hits_files})
  set_tests_properties(qanet_keys_hits_clean PROPERTIES TIMEOUT 30 FIXTURES_SETUP qanet_keys_clean)
  foreach(shard 1 2 3 4 5 6)
    add_test(NAME qanet_keys_${shard}of6 COMMAND ${runner} $<TARGET_FILE:o3os_qanet_keys> "--test-case=qanet keys: every bound*")
    set_tests_properties(qanet_keys_${shard}of6 PROPERTIES TIMEOUT 900 ENVIRONMENT "QANET_SHARD=${shard}/6;QANET_HITS=${qanet_hits_dir}/hits_${shard}.txt" FAIL_REGULAR_EXPRESSION "test cases: +0 "
      FIXTURES_SETUP qanet_keys_shards FIXTURES_REQUIRED qanet_keys_clean)
  endforeach()
  file(MAKE_DIRECTORY ${qanet_hits_dir})
  add_test(NAME qanet_keys_patterns COMMAND ${runner} $<TARGET_FILE:o3os_qanet_keys> "--test-case=qanet keys: every pattern*")
  string(REPLACE ";" "|" qanet_hits_env "${qanet_hits_files}")
  set_tests_properties(qanet_keys_patterns PROPERTIES TIMEOUT 60 ENVIRONMENT "QANET_HITS_FILES=${qanet_hits_env}" FAIL_REGULAR_EXPRESSION "test cases: +0 " FIXTURES_REQUIRED qanet_keys_shards)
  add_test(NAME qanet_keys_selftest COMMAND ${runner} $<TARGET_FILE:o3os_qanet_keys> "--test-case-exclude=qanet keys: every bound*,qanet keys: every pattern*")
  set_tests_properties(qanet_keys_selftest PROPERTIES TIMEOUT 300 FAIL_REGULAR_EXPRESSION "test cases: +0 ")
  # OS-QANET A3: an operation on a selection acts on the selection (tests/qanet/selection_ops.cpp is the table and the check, in the library above so that part B can call it; selection_test.cpp is the walk).
  add_executable(o3os_qanet_select qanet/selection_test.cpp)
  target_link_libraries(o3os_qanet_select PRIVATE o3os_qanet_rules_lib)
  target_include_directories(o3os_qanet_select SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party)
  target_link_libraries(o3os_qanet_select PRIVATE
    core usb hal eve model formats platform_host midi seq module o3m_builtin ui app sim formats_wav Threads::Threads ${CMAKE_DL_LIBS})
  o3os_strict_warnings(o3os_qanet_select)
  add_dependencies(o3os_qanet_select e2e_card_fixture)
  target_compile_definitions(o3os_qanet_select PRIVATE
    O3_QANET_SELECT_BASELINE="${CMAKE_CURRENT_SOURCE_DIR}/qanet/baseline_select.txt"
    O3_QANET_ALLOW="${CMAKE_CURRENT_SOURCE_DIR}/qanet/allow_rules.txt")
  foreach(shard 1 2 3)
    add_test(NAME qanet_select_${shard}of3 COMMAND ${runner} $<TARGET_FILE:o3os_qanet_select> "--test-case=qanet select: every operation*")
    set_tests_properties(qanet_select_${shard}of3 PROPERTIES TIMEOUT 900 ENVIRONMENT "QANET_SHARD=${shard}/3" FAIL_REGULAR_EXPRESSION "test cases: +0 ")
  endforeach()
  add_test(NAME qanet_select_selftest COMMAND ${runner} $<TARGET_FILE:o3os_qanet_select> "--test-case-exclude=qanet select: every operation*")
  set_tests_properties(qanet_select_selftest PROPERTIES TIMEOUT 300 FAIL_REGULAR_EXPRESSION "test cases: +0 ")
endif()

# OS-QANET A4: the ledger of the bugs a person found (docs/qa/qanet-ledger.md) against the tests and baselines that hold them. It lists the build's ctest names (`ctest -N`, nothing runs), so it
# is registered only where the tests it names are (the rules, keys, select and audio matrix binaries). Pure Python and under a second: label `static` (cmake/O3osLabels.cmake).
if(TARGET o3os_qanet_rules AND TARGET o3os_qanet_keys AND TARGET o3os_qanet_select AND TARGET o3os_qanet_audio)
  add_test(NAME qanet_ledger COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/qanet_ledger.py --check --root ${PROJECT_SOURCE_DIR} --ctest-dir ${CMAKE_BINARY_DIR} --ctest ${CMAKE_CTEST_COMMAND})
  add_test(NAME qanet_ledger_selftest COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/qanet_ledger.py --self-test)
  set_tests_properties(qanet_ledger qanet_ledger_selftest PROPERTIES TIMEOUT 60)
endif()

# OS-QANET F1: the planted bugs (tests/qanet/mutants/*.patch) and their tool. This is only the dry run: every patch has its header (ID, class, twin, expected part), patches exactly one product file, the parser
# and the kill table work on a canned ctest output. The run that applies each patch in a throwaway worktree and builds is `python3 -I tools/qanet_mutants.py --run` (tests/qanet/README.md): on demand, never in ci.sh host.
add_test(NAME qanet_mutants_selftest COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/qanet_mutants.py --self-test --root ${PROJECT_SOURCE_DIR})
set_tests_properties(qanet_mutants_selftest PROPERTIES TIMEOUT 60)

# OS-REVIEW: the unit's start_app() wires the App with every hook that the simulator (and app.h) has (0.4.1 forgot attach_param_ring: the Mixer
# moved on the screen and not in the sound, with every host test green).
add_test(NAME wiring_audit COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/wiring_audit.py ${PROJECT_SOURCE_DIR})
add_test(NAME wiring_audit_selftest COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/wiring_audit.py --self-test ${PROJECT_SOURCE_DIR})
set_tests_properties(wiring_audit wiring_audit_selftest PROPERTIES TIMEOUT 60)

# OS-MANUALTOOLS: tools/manual_check.py and tools/manual_llms.py on a temporary repository (needs git).
add_test(NAME manual_tools_selftest COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/manual_tools_test.py ${PROJECT_SOURCE_DIR})
set_tests_properties(manual_tools_selftest PROPERTIES TIMEOUT 60)

# OS-MANUAL2 M2-2: tools/manual_tables.py (the chapters' and the cheat sheet's key tables from the firmware's key-row tables) on a scratch copy of its sources.
add_test(NAME manual_tables_selftest COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/manual_tables.py --self-test --root ${PROJECT_SOURCE_DIR})
set_tests_properties(manual_tables_selftest PROPERTIES TIMEOUT 60)

# OS-MANUALDIAG: the manual's SVG diagrams (docs/manual/img/*.svg) against a fresh run of tools/manual_diagrams.py: the chord diagrams take their keys from
# app/src/bindings.cpp and the boot chord and Power hold constants, the screen map from the bindings and the screen registry, the signal flow from the audio
# graph's symbols. Fails when one is stale, missing, unlisted, too big or not in its chapter. Pure Python, no build: cheap, so `static` (every scope).
add_test(NAME manual_diagrams COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/manual_diagrams.py --check --root ${PROJECT_SOURCE_DIR})
set_tests_properties(manual_diagrams PROPERTIES TIMEOUT 60)

# OS-STARTER: tools/release/make_starter.py (the starter-content zip) and its generators, with no network: the generators twice, the zip built twice offline
# from fake downloads (byte-identical), the licence and hash checks, and the demo projects through the C++ reader (o3pj_tool resave, byte for byte).
add_test(NAME starter_python COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/starter_test.py ${PROJECT_SOURCE_DIR} $<TARGET_FILE:o3pj_tool>)
set_tests_properties(starter_python PROPERTIES TIMEOUT 120)

# OS-HILCARD: tools/testdata/testdata.py (the test data kept out of git: a release asset plus third-party files fetched by hash), with no network: the committed
# manifest, the validator's refusals, the asset built twice from fake sources (byte-identical) and unpacked, a wrong hash refused before anything is written.
add_test(NAME testdata_python COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/testdata_test.py ${PROJECT_SOURCE_DIR})
set_tests_properties(testdata_python PROPERTIES TIMEOUT 60)

# OS-008a: the MED reader against an independent Python writer. tools/mmd_build.py writes reference modules (MMD0 to MMD3)
# with the dump it expects of each; o3med_tool dumps what the C++ reader reads out of the same bytes; the two must agree.
add_executable(o3med_tool tools/o3med_tool.cpp)
target_link_libraries(o3med_tool PRIVATE formats)
o3os_strict_warnings(o3med_tool)
add_test(NAME med_python
  COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/mmd_build.py --check $<TARGET_FILE:o3med_tool>
          ${CMAKE_CURRENT_BINARY_DIR}/med_python)
set_tests_properties(med_python PROPERTIES TIMEOUT 120)
add_test(NAME med_python_selftest COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/mmd_build.py --self-test)
# OS-008b: the same reference modules through the importer (`o3med_tool import`): each is taken, with the song name and, per instrument,
# the result and the frames of PCM that mmd_build.py works out from its own description.
add_test(NAME med_import_python
  COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/mmd_build.py --import-check $<TARGET_FILE:o3med_tool>
          ${CMAKE_CURRENT_BINARY_DIR}/med_import_python)
set_tests_properties(med_import_python PROPERTIES TIMEOUT 120)

# OS-008b: the importer against libopenmpt (a second opinion on notes, volumes, pitch, loops, tempo and flow). o3med_tool imports synthetic
# modules from tools/mmd_build.py; the script loads libopenmpt with ctypes from the system when there is one (nothing is vendored, no
# headers needed) and exits 77, which ctest reports as skipped, when there is not.
add_test(NAME med_openmpt
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/med_openmpt_oracle.py $<TARGET_FILE:o3med_tool>
          ${CMAKE_CURRENT_BINARY_DIR}/med_openmpt)
set_tests_properties(med_openmpt PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

# OS-003f, the boot logo as its own verdict in the ctest summary: the boot screen (words, flash budget, hold, pixels), the picture
# over the FT812 driver, and the simulator's bitmap transform it relies on. The converter's own tests are in ui/CMakeLists.txt.
add_test(NAME boot_logo COMMAND ${runner} $<TARGET_FILE:o3os_tests> "--test-case=boot *,display boot*,transform:*,idle hook*")
set_tests_properties(boot_logo PROPERTIES TIMEOUT 120 FAIL_REGULAR_EXPRESSION "test cases: +0 ")

# OS-MODIMP: the MOD / S3M reader against an independent Python writer. tools/mod_build.py writes reference modules (every MOD variant, S3M with its instrument
# kinds, markers, panning and channel settings) with the dump it expects of each; o3mod_tool dumps what the C++ reader reads out of the same bytes; the two must agree.
add_executable(o3mod_tool tools/o3mod_tool.cpp)
target_link_libraries(o3mod_tool PRIVATE formats)
o3os_strict_warnings(o3mod_tool)
add_test(NAME mod_python
  COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/mod_build.py --check $<TARGET_FILE:o3mod_tool>
          ${CMAKE_CURRENT_BINARY_DIR}/mod_python)
set_tests_properties(mod_python PROPERTIES TIMEOUT 120)
add_test(NAME mod_python_selftest COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/mod_build.py --self-test)
# OS-FINALQA FQA-4: the picture sheets of the QA catalogue (tools/qa_sheet.py) check their own cases: a planted catalogue and picture directory.
add_test(NAME qa_sheet_python_selftest COMMAND ${Python3_EXECUTABLE} -I ${PROJECT_SOURCE_DIR}/tools/qa_sheet.py --self-test)
set_tests_properties(qa_sheet_python_selftest PROPERTIES TIMEOUT 60)

# OS-MODIMP: the MOD and S3M importers against libopenmpt (a second opinion on the decode of every cell, pitch, loops, flow, timing, channels over 16 and volume
# persistence), through `o3mod_tool import` and the synthetic modules of tools/mod_build.py. Loaded with ctypes from the system when there is one; exits 77 (skipped)
# when there is not, and fails under O3_CI=1 (tools/ci.sh) so that CI cannot go quiet.
# OS-COREFX CF1: the tracker FX against libopenmpt in the AUDIO: o3corefx_tool plays a synthetic module through the importer, the sequencer and the Modern engine (tracker FX processor, voices, graph)
# and writes the output; tests/corefx_openmpt_oracle.py renders the same bytes with libopenmpt and compares the pitch and the level of every tick.
add_executable(o3corefx_tool tools/o3corefx_tool.cpp)
target_link_libraries(o3corefx_tool PRIVATE formats seq midi model audio core hal usb)
o3os_strict_warnings(o3corefx_tool)
# OS-E2E-9: the original Open303 (double precision) as a command-line reference for tests/e2e_open303_ref.py, which compares it with the TB-303 journey's render (sim/scenarios/e2e/e2e_ref_303.txt).
add_executable(o3ref303_tool tools/o3ref303_tool.cpp ${oracle_sources})
target_link_libraries(o3ref303_tool PRIVATE open303_oracle)
o3os_strict_warnings(o3ref303_tool)

add_test(NAME corefx_openmpt
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/corefx_openmpt_oracle.py $<TARGET_FILE:o3corefx_tool> ${CMAKE_CURRENT_BINARY_DIR}/corefx_openmpt)
set_tests_properties(corefx_openmpt PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

add_test(NAME modimp_openmpt
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/modimp_openmpt_oracle.py $<TARGET_FILE:o3mod_tool>
          ${CMAKE_CURRENT_BINARY_DIR}/modimp_openmpt)
set_tests_properties(modimp_openmpt PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

o3os_label_tests()
