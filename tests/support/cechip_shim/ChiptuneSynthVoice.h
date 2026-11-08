// SPDX-License-Identifier: GPL-3.0-or-later
//
// The oracle shim of OS-CECHIP C1: the header named "ChiptuneSynthVoice.h" that tracker-ce's ChiptuneSynthVoice.cpp includes. Ours; the original
// needs only the one factory below. Host only.
#pragma once

#include <memory>

#include "PolySynth/sdk/SynthVoiceSDK.h"

namespace polysynth {

// The factory the original defines at the end of its file: an InstrumentType named "CHIP" with createMetadata and createVoice.
std::unique_ptr<InstrumentType> createChiptuneSynth();

}  // namespace polysynth
