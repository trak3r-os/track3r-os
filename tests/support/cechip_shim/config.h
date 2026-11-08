// SPDX-License-Identifier: GPL-3.0-or-later
//
// The oracle shim of OS-CECHIP C1 (see ../cechip_ref.h): the header named "config.h" that tracker-ce's ChiptuneSynthVoice.cpp includes first.
// Ours, written from the facts in docs/specs/OS-CECHIP.md; nothing of tracker-ce's SDK is copied. Host only.
#pragma once

// The original's whole body sits inside `#ifdef FEATURE_SYNTH`.
#define FEATURE_SYNTH 1
