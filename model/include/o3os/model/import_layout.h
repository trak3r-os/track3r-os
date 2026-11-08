// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OS-FLASHDIET I1/I2 (Level C: published module ABI): the importer's layout tag. A kind 3 module (an importer, O3M_KIND_IMPORTER, ABI 1.11) is
// compiled against THIS tree's model headers and writes the project DIRECTLY through the arena pointer the OS hands it at o3m_importer.begin
// (formats/mod and formats/med call the Project mutators and read Pattern / Step / Instrument inline: nothing goes through a host call), so the
// bytes it writes are laid out as this tree's model structs are. The packer (tools/o3m_pack.py) writes the tag of the layout the module was built
// against into header word 44 of its .tmod, and the loader refuses a kind 3 file whose word 44 is not this OS's own tag with
// Status::LayoutMismatch: a module built against a different model layout never runs against this OS, whatever its ABI minor says.
//
// The tag therefore hashes the layout of everything the importer writes directly: ModelArena and Project (sizeof and the offset of every
// member, which pins every embedded struct's position and size) plus the member layouts of the structs it writes field by field (TrackSlot,
// Pattern, Step, Instrument and its SamplerParams / Envelope / Lfo), plus a manual version number. Only layouts that are identical on the
// 64-bit host and the Cortex-M4F enter the hash (Project is static_asserted alignof 4, so no member is a pointer or a 64-bit word): the packer
// builds with the cross compiler, the host tests with the host one, and both must compute the same number. The import report and options
// blobs that cross at report()/begin() hold pointers and 64-bit counts, so their raw layout is host/target-unstable by design; they are covered
// by kImportLayoutVersion instead, bumped by hand when one of them changes shape.
//
// This header is the OS's side of the contract; module/sdk/o3module.h documents the word but never includes this file (the SDK stays MIT
// licensed and standalone).

#include <o3os/model/instrument.h>
#include <o3os/model/project.h>
#include <o3os/model/step.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace o3os::model {

// One round of the tag's hash: FNV-1a's offset basis, Knuth's multiplicative hash.
constexpr std::uint32_t layout_mix(std::uint32_t h, std::uint32_t v) noexcept { return (h ^ v) * 2654435761u; }

// Mixes a list of layout facts (sizes and offsetof()s) into `h`.
constexpr std::uint32_t layout_mix_all(std::uint32_t h, std::initializer_list<std::uint32_t> facts) noexcept {
  for (const std::uint32_t v : facts) {
    h = layout_mix(h, v);
  }
  return h;
}

// The tag of a step layout given its facts: size, the offsets of note, instr, vol and fx, and the size of one FX pair. Pass offsetof()s of the
// struct, so a copy of Step with a field moved gives a different tag.
constexpr std::uint32_t step_layout_tag(std::uint32_t size, std::uint32_t off_note, std::uint32_t off_instr, std::uint32_t off_vol,
                                        std::uint32_t off_fx, std::uint32_t fx_size) noexcept {
  return layout_mix_all(2166136261u, {size, off_note, off_instr, off_vol, off_fx, fx_size});
}

// OS-FLASHDIET I2 (review note 2): the manual version of the layouts that cross the module boundary but cannot be hashed raw (they hold
// pointers / 64-bit counts, so their layout differs between the host and the target): the import report and options blobs of
// formats/import_module.h. Bump when one of them changes shape.
inline constexpr std::uint32_t kImportLayoutVersion = 2;

// The Project facts, read through the friend declared in project.h (its members are private).
constexpr std::uint32_t model_import_layout_probe(std::uint32_t h) noexcept {
  return layout_mix_all(h, {sizeof(Project), offsetof(Project, name_), offsetof(Project, swing_), offsetof(Project, default_pattern_length_),
                            offsetof(Project, active_tracks_), offsetof(Project, song_length_), offsetof(Project, pattern_count_),
                            offsetof(Project, instrument_count_), offsetof(Project, module_count_), offsetof(Project, names_state_),
                            offsetof(Project, unknown_filling_), offsetof(Project, tempo_x100_), offsetof(Project, song_), offsetof(Project, master_),
                            offsetof(Project, tracks_), offsetof(Project, slot_of_id_), offsetof(Project, modules_), offsetof(Project, unknown_),
                            offsetof(Project, state_present_), offsetof(Project, volume_law_), offsetof(Project, app_params_count_),
                            offsetof(Project, midi_), offsetof(Project, usb_), offsetof(Project, mix_), offsetof(Project, perf_), offsetof(Project, note_),
                            offsetof(Project, mute_), offsetof(Project, scenes_), offsetof(Project, send_fx_), offsetof(Project, inserts_),
                            offsetof(Project, app_params_), offsetof(Project, unknown_count_), offsetof(Project, unknown_used_),
                            offsetof(Project, unknown_filled_), offsetof(Project, unknown_dropped_chunks_), offsetof(Project, unknown_dropped_bytes_),
                            offsetof(Project, generation_), offsetof(Project, tracks_generation_), offsetof(Project, song_generation_),
                            offsetof(Project, order_epoch_), offsetof(Project, order_ops_), offsetof(Project, order_open_),
                            offsetof(Project, order_journal_), offsetof(Project, order_depth_), offsetof(Project, pattern_generation_),
                            offsetof(Project, instrument_generation_), offsetof(Project, param_generation_), offsetof(Project, param_epoch_),
                            offsetof(Project, settings_), offsetof(Project, unknown_pool_), offsetof(Project, instruments_), offsetof(Project, patterns_)});
}

// The tag of THIS OS's import-facing model layout: header word 44 of a kind 3 .tmod built against this tree. Pinned by
// tests/module_importer_test.cpp; the build hands the same number to tools/o3m_pack.py (--layout-tag, printed by a host-compiled one-liner
// over this header), and o3m_pack.py's own mirror pins it for its --self-test.
inline constexpr std::uint32_t kImportLayoutTag = []() constexpr {
  std::uint32_t h = 2166136261u;
  h = layout_mix(h, kImportLayoutVersion);
  h = layout_mix_all(h, {sizeof(ModelArena), offsetof(ModelArena, project)});
  h = model_import_layout_probe(h);
  h = layout_mix_all(h, {sizeof(TrackSlot), offsetof(TrackSlot, type), offsetof(TrackSlot, flags), offsetof(TrackSlot, volume), offsetof(TrackSlot, pan),
                         offsetof(TrackSlot, bus), offsetof(TrackSlot, name)});
  h = layout_mix_all(h, {sizeof(Pattern), offsetof(Pattern, id), offsetof(Pattern, track_length), offsetof(Pattern, name), offsetof(Pattern, steps)});
  h = layout_mix_all(h, {sizeof(Step), offsetof(Step, note), offsetof(Step, instr), offsetof(Step, vol), offsetof(Step, fx), sizeof(StepFx)});
  h = layout_mix_all(h, {sizeof(Instrument), offsetof(Instrument, kind), offsetof(Instrument, midi_channel), offsetof(Instrument, module_index),
                         offsetof(Instrument, engine), offsetof(Instrument, param_len), offsetof(Instrument, name), offsetof(Instrument, sample_path),
                         offsetof(Instrument, sample_rate), offsetof(Instrument, sampler), offsetof(Instrument, params),
                         offsetof(Instrument, midi_velocity), offsetof(Instrument, midi_program), offsetof(Instrument, midi_out),
                         offsetof(Instrument, midi_pb_range), offsetof(Instrument, midi_offset), offsetof(Instrument, midi_cc),
                         offsetof(Instrument, vel_sens), offsetof(Instrument, stereo), offsetof(Instrument, reserved)});
  h = layout_mix_all(h, {sizeof(SamplerParams), offsetof(SamplerParams, cutoff), offsetof(SamplerParams, resonance), offsetof(SamplerParams, envelope),
                         offsetof(SamplerParams, lfo), offsetof(SamplerParams, wavetable_window), offsetof(SamplerParams, start),
                         offsetof(SamplerParams, loop_start), offsetof(SamplerParams, loop_end), offsetof(SamplerParams, end),
                         offsetof(SamplerParams, slices), offsetof(SamplerParams, glide), offsetof(SamplerParams, wavetable_window_size),
                         offsetof(SamplerParams, grain_length), offsetof(SamplerParams, grain_position), offsetof(SamplerParams, sample_type),
                         offsetof(SamplerParams, play_mode), offsetof(SamplerParams, filter_type), offsetof(SamplerParams, filter_enable),
                         offsetof(SamplerParams, tune), offsetof(SamplerParams, fine_tune), offsetof(SamplerParams, volume),
                         offsetof(SamplerParams, panning), offsetof(SamplerParams, delay_send), offsetof(SamplerParams, reverb_send),
                         offsetof(SamplerParams, overdrive), offsetof(SamplerParams, bit_depth), offsetof(SamplerParams, slice_count),
                         offsetof(SamplerParams, selected_slice), offsetof(SamplerParams, grain_shape), offsetof(SamplerParams, grain_type)});
  h = layout_mix_all(h, {sizeof(Envelope), offsetof(Envelope, amount), offsetof(Envelope, delay), offsetof(Envelope, attack), offsetof(Envelope, hold),
                         offsetof(Envelope, decay), offsetof(Envelope, sustain), offsetof(Envelope, release), offsetof(Envelope, loop),
                         offsetof(Envelope, enable)});
  h = layout_mix_all(h, {sizeof(Lfo), offsetof(Lfo, shape), offsetof(Lfo, speed), offsetof(Lfo, amount)});
  return h;
}();

}  // namespace o3os::model
