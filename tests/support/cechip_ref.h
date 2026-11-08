// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The oracle of the CHIP engine (OS-CECHIP C1): a plain C++17 interface over tracker-ce's ChiptuneSynthVoice.cpp (third_party/tracker-ce-chip, MIT,
// Copyright (c) 2020-2026 Polyend Sp. z o.o. and Polyend Tracker Community Edition Contributors; THIRD_PARTY.md), compiled UNMODIFIED against the
// shim in tests/support/cechip_shim. A test sees only these types: none of the original's or the shim's names. Host only, never linked into an image.
//
// The wrapper calls what the original exposes: its metadata builder (the parameters' names, defaults and ranges), the captured setters (one
// per parameter, called with CE's own values: Hz, seconds, semitones, bits), onNoteOn, onNoteOff and renderVoiceAudio. Mono (channel 0), at kSampleRate.

#include <cstddef>

namespace cechip_ref {

// CE's parameter ids, in the original's order (docs/specs/OS-CECHIP.md F2).
enum Id : int {
  kModel = 0, kTune, kFine, kShape, kPwmRate, kPwmDepth, kSub, kChord, kArpSpeed, kSweep, kSweepTime, kVibRate, kVibDepth,
  kAttack, kDecay, kSustain, kRelease, kCutoff, kResonance, kEnvAmt, kBitcrush, kDecimate, kGlideMode, kGlideTime, kVolume,
  kParamCount
};

constexpr int kModelCount = 8;  // 2A03 Pulse, SID Saw/Tri, Noise / LFSR, FM Bell, Super Square, Zap / Laser, Arp Chord, Chip Drum

// The sample rate the original runs at.
float sample_rate();

// The original's description of its parameters (from its own metadata builder).
int param_count();
const char* param_name(int id);
float param_default(int id);
float param_min(int id);
float param_max(int id);

// One CE voice, created with the original's defaults.
class Voice {
 public:
  Voice();
  ~Voice();
  Voice(const Voice&) = delete;
  Voice& operator=(const Voice&) = delete;

  // Sets one parameter to CE's value (what the original's setter receives).
  void set(int id, float ce_value);
  // onNoteOn with the MIDI note; `legato` is the original's isLegatoStep. The original ignores velocity.
  void note_on(int note, float velocity = 1.0f, bool legato = false);
  void note_off(int note);
  bool active();
  // renderVoiceAudio into out[0..n): the mono channel; the other channel and the aux buffers are empty.
  void render(float* out, std::size_t n);

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace cechip_ref
