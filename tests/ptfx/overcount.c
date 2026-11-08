/* SPDX-License-Identifier: GPL-3.0-or-later */
/* OS-CEPTFX S0 test plugin `overcount`: returns more frames than it was asked for (A11: the run must fail). */
#include "ptfx_plugin_abi.h"

void plugin_init(void* scratch, uint32_t scratch_size, const PtfxTestHostApi* host) {
  (void)scratch;
  (void)scratch_size;
  (void)host;
}

void plugin_set_param(uint8_t index, float value) {
  (void)index;
  (void)value;
}

int32_t plugin_process_block(const int16_t** in, int16_t** out, uint32_t samples, uint8_t channels) {
  for (uint8_t c = 0; c < channels; ++c) {
    const int16_t* src = in[c];
    int16_t* dst = out[c];
    if (src == 0 || dst == 0) {
      continue;
    }
    for (uint32_t i = 0; i < samples; ++i) {
      dst[i] = src[i];
    }
  }
  return (int32_t)samples + 1;
}
